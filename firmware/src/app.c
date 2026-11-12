/* =========================================================================
 * app.c —— 键盘应用层
 *
 *   1ms 节拍：扫描矩阵 → 编码器解码 → 生成 HID 报文 → 灯效刷新
 *   Feature 命令在 USB 中断里执行（只做内存拷贝，不写 Flash）
 *
 * 矩阵约定（务必与原理图一致）：
 *   列 = 推挽输出，逐列拉低
 *   行 = 准双向输入，靠内部上拉；按键按下时行被拉低
 *   二极管阳极接轴、阴极接列 → 只能"列低、行读"，反过来永远读不到
 * ========================================================================= */
#include "board.h"
#include "usb.h"
#include "app.h"

/* ============================ 全局状态 ============================ */
__xdata keyboard_config_t cfg;
__xdata uint8_t  led_buf[3 * NUM_LEDS];       /* GRB 顺序，WS2812 要求 */

static __code const char DevName[] = "KeyForge CH552";

static volatile uint8_t tick_flag;

static __xdata uint16_t matrix_raw;            /* 当前扫描结果 */
static __xdata uint16_t matrix_stable;         /* 消抖后的状态 */
static __xdata uint8_t debounce_cnt[MAT_BITS];   /* 放 xRAM：内部 RAM 只剩 ~120B 给 DSEG */

static __xdata int8_t   enc_acc;                 /* 4 倍频累计 */
static __xdata uint8_t  enc_prev;
static __xdata int8_t   enc_delta;               /* 本拍累积的刻度数 */
static __xdata int8_t   enc_net;                 /* 待上报的净刻度（清空于状态包） */
static __xdata uint16_t enc_pulse;             /* 旋钮"按键化"脉冲的功能码 */
static __xdata uint8_t  enc_pulse_cnt;
static __xdata uint8_t  enc_push_state;
/* 待发的主机动作编号。0xFF = 没有待发动作。
 * 不能拿 0 当"没有"——动作编号 0 是合法的（配置器第一个槽位），
 * 用 0 当空值会导致"槽位 0 永远发不出去"。 */
#define HOST_ACTION_NONE 0xFF
static __xdata uint8_t host_action_pending = HOST_ACTION_NONE;

static __xdata uint16_t active_sw[NUM_KEYS];   /* 每个轴当前生效的功能码 */
static __xdata uint16_t active_enc_push;

/* 光标移动的连击提速状态（全局共享：一般只会按一个方向键） */
static __xdata uint8_t  move_edge;             /* 有 T_MOVE 键新按下（按下沿） */
static __xdata uint8_t  move_taps;             /* 连击次数 1..3 → 倍率 */
static __xdata uint8_t  move_gap;              /* 松手后经过的 20ms 拍数 */

static __xdata uint8_t last_kbd[8];
static __xdata uint16_t last_media;
static __xdata uint8_t last_mouse[4];

static __xdata uint16_t led_t;                 /* 灯效时间（20ms 步进） */
static __xdata uint8_t save_pending;
static __xdata uint8_t layer_fn;

/* 矩阵 bit → 键索引（-1 表示编码器按压）
 *
 * 2026-11-11 按实测接线修正：ROW1 = 上排、ROW2 = 中排、ROW3 = 下排，COL1..COL4 左→右。
 * bit3 = (R1,C4) 正好是右上角的旋钮，与实物一致。
 *
 *   bit0..2   R1C1..R1C3 → 上排 CN9 CN10 CN11
 *   bit3      R1C4       → 旋钮按压
 *   bit4..7   R2C1..R2C4 → 中排 CN5 CN6 CN7 CN8
 *   bit8..11  R3C1..R3C4 → 下排 CN1 CN2 CN3 CN4
 *
 * 所以键索引 = CN 编号 - 1，配置器的 LAYOUT 与这张表必须保持一致。
 */
static __code const int8_t bit_to_key[MAT_BITS] = { 8, 9, 10, -1, 4, 5, 6, 7, 0, 1, 2, 3 };

/* =========================================================================
 * 自保入口：跳回芯片 ROM 里的 BootLoader（0x3800）
 *
 * 为什么非要这个：
 *   flash 配置信息 bit14 = No_Boot_Load，如果它是 0，芯片上电**直接跳过
 *   BootLoader** 去跑用户程序（手册表 6.2："0-从 0000h 地址的应用程序启动"）。
 *   一旦这样，USB 和串口都进不去下载模式 —— 从外部无论怎么接、怎么复位、
 *   怎么拉 D+/P1.5 都没用，因为没有一个引脚会被采样。
 *
 *   WCH 官方论坛（《CH552 如何从用户代码区返回到 bootloader》）确认：
 *   用户程序执行 LJMP 0x3800 即可回到 BootLoader。
 *
 *   所以固件里必须留这条后门：长按旋钮 3 秒 = 无条件回到烧录模式。
 *   只要 MCU 还在跑，哪怕 USB 全挂、哪怕配置位被写坏，都能救回来。
 * ========================================================================= */
static void enter_bootloader(void)
{
    EA_DISABLE();

    /* 先断开 D+ 上拉 / 关掉 USB 端口，让主机看到一次"拔出"，
       这样 BootLoader 重新跑 USB 初始化时是一次干净的重枚举。 */
    USB_CTRL  &= (uint8_t)~bUC_DEV_PU_EN;
    UDEV_CTRL &= (uint8_t)~bUD_PORT_EN;
    {
        volatile uint16_t d = 30000;      /* 给主机约 20ms 检测断开 */
        while (d--) { }
    }

    /* 永不返回 */
    __asm
        ljmp    0x3800
    __endasm;
}

/* ============================ 默认配置 ============================ */
/* 可用配置器"导出固件默认表"覆盖这一段 */
/* 出厂键位（下标 = 配置器里的键位 id = bit_to_key 的键索引）
 *
 * 板面（按丝印 CN 编号，与配置器网格一致）：
 *   上排   CN9=9     CN10=0    CN11=Backspace   [旋钮 SW1：旋转 音量±，按下 静音]
 *   中排   CN5=5     CN6=6     CN7=7            CN8=8
 *   下排   CN1=1     CN2=2     CN3=3            CN4=4
 *
 * 编码规则：0x01xx = 普通键(usage)，0x5MKK = 组合键（M=修饰位，KK=usage，
 *           例：Win(0x08)+R(0x15) = 0x5815），0x02xx = 多媒体 usage，
 *           0x07xx = 鼠标相对移动（1上 2下 3左 4右，按住持续发送）。
 */
static __code const uint16_t DEF_KEYS[NUM_KEYS] = {
    0x011E, 0x011F, 0x0120, 0x0121, /* CN1 1, CN2 2, CN3 3, CN4 4 */
    0x0122, 0x0123, 0x0124, 0x0125, /* CN5 5, CN6 6, CN7 7, CN8 8 */
    0x0126, 0x0127, 0x012A          /* CN9 9, CN10 0, CN11 Backspace */
};
/* 多媒体码必须是 HID Consumer 的真实 usage，不能是自定义序号：
   音量+ = 0xE9，音量- = 0xEA，静音 = 0xE2（旋钮按下）。
   之前写成 0x01/0x02/0x04，主机不认，所以旋钮完全没反应。 */
static __code const uint16_t DEF_ENC[3] = { 0x02E9, 0x02EA, 0x02E2 };

/* ============================ DataFlash ============================ */
/* CH552 DataFlash 128 字节，按字节改写，地址 0xC000 + (off<<1) */
static void df_write(uint8_t off, uint8_t val)
{
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    GLOBAL_CFG |= 0x04;                 /* bDATA_WE */
    ROM_ADDR_H = 0xC0;
    ROM_ADDR_L = (uint8_t)(off << 1);
    ROM_DATA_L = val;
    ROM_CTRL = 0x9A;                    /* 写命令，执行期间 CPU 自动暂停 */
    SAFE_MOD = 0x00;
    GLOBAL_CFG &= (uint8_t)~0x04;
}

static uint8_t df_read(uint8_t off)
{
    ROM_ADDR_H = 0xC0;
    ROM_ADDR_L = (uint8_t)(off << 1);
    ROM_CTRL = 0x8E;                    /* 读命令 */
    return ROM_DATA_L;
}

/* ============================ 配置 ============================ */
static uint8_t cfg_checksum(void)
{
    uint8_t i, x = 0;
    uint8_t *p = (uint8_t *)&cfg;
    for (i = 0; i < (uint8_t)(sizeof(cfg) - 1); i++) x ^= p[i];
    return x;
}

static void cfg_defaults(void)
{
    uint8_t i;
    cfg.magic = CFG_MAGIC;
    cfg.version = CFG_VERSION;
    cfg.enc_invert = 0;
    for (i = 0; i < NUM_KEYS; i++) cfg.keys[i] = DEF_KEYS[i];
    cfg.enc[0] = DEF_ENC[0];
    cfg.enc[1] = DEF_ENC[1];
    cfg.enc[2] = DEF_ENC[2];
    cfg.led_mode = 1;               /* 呼吸 */
    cfg.led_bright = 30;
    cfg.led_speed = 50;
    cfg.led_rgb[0] = 0xF7;          /* #F77634 */
    cfg.led_rgb[1] = 0x76;
    cfg.led_rgb[2] = 0x34;
    for (i = 0; i < NUM_LEDS; i++) {
        cfg.per_key[i][0] = 0xF7;
        cfg.per_key[i][1] = 0x76;
        cfg.per_key[i][2] = 0x34;
    }
    cfg.checksum = cfg_checksum();
}

static void cfg_load(void)
{
    uint8_t i;
    uint8_t *p = (uint8_t *)&cfg;
    for (i = 0; i < (uint8_t)sizeof(cfg); i++) p[i] = df_read(i);

    if (cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION || cfg.checksum != cfg_checksum()) {
        cfg_defaults();
    }
}

static void cfg_save(void)
{
    uint8_t i;
    uint8_t *p = (uint8_t *)&cfg;
    cfg.checksum = cfg_checksum();
    EA_DISABLE();
    for (i = 0; i < (uint8_t)sizeof(cfg); i++) df_write(i, p[i]);
    EA_ENABLE();
}

/* ============================ 定时器 ============================ */
/* Fsys = 24MHz，T0 时钟 Fsys/12 = 2MHz，1ms = 2000 计数 */
#define T0_RELOAD   (65536u - 2000u)

void timer0_isr(void) __interrupt(INT_NO_TMR0)
{
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)(T0_RELOAD & 0xFF);
    tick_flag = 1;
}

static void timer0_init(void)
{
    TMOD = (uint8_t)((TMOD & 0xF0) | 0x01);   /* T0 方式 1，16 位 */
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)(T0_RELOAD & 0xFF);
    ET0 = 1;
    TR0 = 1;
}

/* ============================ GPIO ============================ */
static void gpio_init(void)
{
    /* P1：P1.4/P1.5 = 行（准双向，有上拉）；P1.1/P1.6/P1.7 = 列（推挽输出） */
    P1_MOD_OC |=  (uint8_t)((1 << 4) | (1 << 5));
    P1_MOD_OC &= (uint8_t)~((1 << 1) | (1 << 6) | (1 << 7));
    P1_DIR_PU |=  (uint8_t)((1 << 1) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7));

    /* P3：P3.0 = 行；P3.2/P3.4 = 编码器（准双向）；
           P3.1 = 列；P3.3 = LED 数据（推挽输出） */
    P3_MOD_OC |=  (uint8_t)((1 << 0) | (1 << 2) | (1 << 4));
    P3_MOD_OC &= (uint8_t)~((1 << 1) | (1 << 3));
    P3_DIR_PU |=  (uint8_t)((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4));

    /* 列初始为高 */
    COL1_PIN = 1; COL2_PIN = 1; COL3_PIN = 1; COL4_PIN = 1;
    LED_PIN = 0;
}

/* ============================ 矩阵扫描 ============================ */
static void matrix_scan(void)
{
    uint16_t v = 0;
    uint8_t c;

    for (c = 0; c < MAT_COLS; c++) {
        /* 全部列拉高 */
        COL1_PIN = 1; COL2_PIN = 1; COL3_PIN = 1; COL4_PIN = 1;
        /* 目标列拉低 */
        switch (c) {
        case 0: COL1_PIN = 0; break;
        case 1: COL2_PIN = 0; break;
        case 2: COL3_PIN = 0; break;
        default: COL4_PIN = 0; break;
        }
        NOP(); NOP(); NOP(); NOP(); NOP(); NOP();

        if (!ROW1_PIN) v |= (uint16_t)(1u << (0 * MAT_COLS + c));
        if (!ROW2_PIN) v |= (uint16_t)(1u << (1 * MAT_COLS + c));
        if (!ROW3_PIN) v |= (uint16_t)(1u << (2 * MAT_COLS + c));
    }

    /* 扫描结束后把列恢复为高，避免按住时持续灌电流 */
    COL1_PIN = 1; COL2_PIN = 1; COL3_PIN = 1; COL4_PIN = 1;
    matrix_raw = v;
}

/* 3 次一致才认为稳定（3ms 消抖） */
static void matrix_debounce(void)
{
    uint8_t b;
    for (b = 0; b < MAT_BITS; b++) {
        uint8_t cur = (matrix_raw >> b) & 1;
        uint8_t old = (matrix_stable >> b) & 1;
        if (cur != old) {
            if (debounce_cnt[b] < 3) debounce_cnt[b]++;
            if (debounce_cnt[b] >= 3) {
                if (cur) matrix_stable |= (uint16_t)(1u << b);
                else     matrix_stable &= (uint16_t)~(1u << b);
                debounce_cnt[b] = 0;
            }
        } else {
            debounce_cnt[b] = 0;
        }
    }
}

/* ============================ 编码器 ============================ */
/* 自保入口的独立计数（不复用 enc_hold_ticks，避免和按键功能互相干扰） */
static __xdata uint16_t boot_ticks;
/* 诊断计数（CMD_DEBUG 读出去，用来判断主循环和 EP1 是否真的在跑） */
static __xdata uint8_t dbg_ticks;
static __xdata uint8_t dbg_tx;
static __code const int8_t enc_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static void encoder_poll(void)
{
    uint8_t s = (uint8_t)((ENC_A_PIN ? 2 : 0) | (ENC_B_PIN ? 1 : 0));
    uint8_t idx = (uint8_t)((enc_prev << 2) | s);
    int8_t d = enc_table[idx & 0x0F];

    enc_prev = s;
    if (d == 0) return;

    enc_acc += d;
    if (enc_acc >= 4) { enc_acc -= 4; enc_delta++; }
    else if (enc_acc <= -4) { enc_acc += 4; enc_delta--; }
}

/* -------------------------------------------------------------------------
 * 自保入口（写死，不依赖任何上层状态）
 *
 * 每 1ms 直接扫一次旋钮的按压脚：把 COL4 拉低、读 ROW1，
 * 不经过矩阵去抖、不经过 matrix_stable、不经过 keys_update、不依赖 USB。
 * 连续 3000 次（= 3 秒）就无条件执行 LJMP 0x3800 回到芯片 ROM 的 BootLoader。
 *
 * 为什么要单独写一份：原来这段逻辑挂在 keys_update() 里，前提是矩阵扫描、
 * 去抖、位映射全都正常。一旦矩阵读不到（比如焊点、方向接反），自保入口
 * 就跟着失效 —— 那就等于没有。这里绕开全部中间环节。
 * ------------------------------------------------------------------------- */
static void boot_hold_poll(void)
{
    uint8_t pressed;

    COL4_PIN = 0;
    NOP(); NOP(); NOP(); NOP();
    pressed = ROW1_PIN ? 0 : 1;
    COL4_PIN = 1;

    if (pressed) {
        if (boot_ticks < 3500) boot_ticks++;
        if (boot_ticks == 3000) enter_bootloader();
    } else {
        boot_ticks = 0;
    }
}

/* ============================ HID 报文 ============================ */
#define T_KEY   0x01
#define T_MEDIA 0x02
#define T_MOUSE 0x03
#define T_ACTION 0x04
#define T_HOST   0x06
#define T_MOVE   0x07          /* 鼠标相对移动：1上 2下 3左 4右 */

static uint8_t code_type(uint16_t c)
{
    uint8_t hi = (uint8_t)(c >> 8);
    if ((hi & 0xF0) == 0x50) return 0x05;     /* 组合键 */
    return hi;
}

static void add_usage(uint8_t *keys, uint8_t *n, uint8_t u)
{
    uint8_t i;
    if (u == 0) return;
    for (i = 0; i < *n; i++) if (keys[i] == u) return;
    if (*n < 6) keys[(*n)++] = u;
}

static void apply_code(uint16_t code, uint8_t *mods, uint8_t *keys, uint8_t *n,
                       uint16_t *media, uint8_t *mouse)
{
    uint8_t t = code_type(code);
    uint8_t v = (uint8_t)(code & 0xFF);

    switch (t) {
    case T_KEY:
        if (v >= 0xE0 && v <= 0xE7) *mods |= (uint8_t)(1u << (v - 0xE0));
        else add_usage(keys, n, v);
        break;
    case 0x05:                                  /* 组合键：高半字节 = 修饰位 */
        *mods |= (uint8_t)((code >> 8) & 0x0F);
        add_usage(keys, n, v);
        break;
    case T_MEDIA:
        if (*media == 0) *media = v;
        break;
    case T_HOST:
        /* 主机动作不产生 HID 键码，由按下沿单独上报（见 keys_update） */
        break;
    case T_MOUSE:
        if (v >= 1 && v <= 3) mouse[0] |= (uint8_t)(1u << (v - 1));
        else if (v == 4) mouse[3] = 1;
        else if (v == 5) mouse[3] = (uint8_t)-1;
        break;
    case T_MOVE:
        /* 相对移动必须"每次按下都发一包"，不能走 build_reports 的变化检测，
           所以这里什么都不做，交给 mouse_move_tick() */
        break;
    default:
        break;
    }
}

/* 鼠标相对移动：按住期间每 MOVE_PERIOD_MS 发一包 [buttons, dx, dy, wheel]。
   X/Y 在 HID 里是有符号字节，往左/上是负数，直接按补码写进 uint8_t 即可。

   速度 = MOVE_STEP << 档位，再乘连击倍率：
     单击后按住    ×1（默认手感）
     快速双击后按住 ×2
     快速三击后按住 ×3
   松手超过 MOVE_TAP_WINDOW 拍（400ms）连击计数清零，回到默认。 */
static void mouse_move_tick(void)
{
    static uint8_t div_move;
    uint8_t i, dx = 0, dy = 0, buf[5];
    uint16_t code;
    uint8_t step;
    uint8_t held = 0;

    if (++div_move < MOVE_PERIOD_MS) return;
    div_move = 0;

    /* ---- 连击判定：这一拍里只要有一个移动键刚按下就算一次"点" ---- */
    if (move_edge) {
        move_edge = 0;
        if (move_gap <= MOVE_TAP_WINDOW) {
            if (move_taps < 3) move_taps++;
        } else {
            move_taps = 1;
        }
        move_gap = 0;
    }

    for (i = 0; i < NUM_KEYS; i++) {
        uint8_t v;
        code = active_sw[i];
        if (code_type(code) != T_MOVE) continue;
        held = 1;
        v = (uint8_t)(code & 0xFF);
        step = (uint8_t)(MOVE_STEP << ((v >> 4) & 0x03));
        step = (uint8_t)(step * (move_taps ? move_taps : 1));
        if (step > MOVE_STEP_MAX) step = MOVE_STEP_MAX;
        switch ((uint8_t)(v & 0x0F)) {
        case 1: dy = (uint8_t)(dy - step); break;   /* 上 */
        case 2: dy = (uint8_t)(dy + step); break;   /* 下 */
        case 3: dx = (uint8_t)(dx - step); break;   /* 左 */
        case 4: dx = (uint8_t)(dx + step); break;   /* 右 */
        default: break;
        }
    }
    if (code_type(active_enc_push) == T_MOVE) {
        uint8_t v = (uint8_t)(active_enc_push & 0xFF);
        held = 1;
        step = (uint8_t)(MOVE_STEP << ((v >> 4) & 0x03));
        step = (uint8_t)(step * (move_taps ? move_taps : 1));
        if (step > MOVE_STEP_MAX) step = MOVE_STEP_MAX;
        switch ((uint8_t)(v & 0x0F)) {
        case 1: dy = (uint8_t)(dy - step); break;
        case 2: dy = (uint8_t)(dy + step); break;
        case 3: dx = (uint8_t)(dx - step); break;
        case 4: dx = (uint8_t)(dx + step); break;
        default: break;
        }
    }

    /* 没按着移动键就累计"松手时间"，超过窗口连击清零 */
    if (!held) {
        if (move_gap < 255) move_gap++;
        if (move_gap > MOVE_TAP_WINDOW) move_taps = 0;
    }

    if (dx == 0 && dy == 0) return;

    buf[0] = REP_MOUSE;
    buf[1] = 0;                 /* 不带按键 */
    buf[2] = dx;
    buf[3] = dy;
    buf[4] = 0;                 /* 不带滚轮 */
    usb_ep1_send(buf, 5);
}

/* 返回 1 表示键盘报文有变化 */
/* 报文拼装用的暂存区：放 xRAM，避免 20 字节压进只剩 94B 的内部 RAM */
static __xdata uint8_t rep_keys[6];
static __xdata uint8_t rep_mouse[4];
static __xdata uint8_t rep_buf[10];

static uint8_t build_reports(void)
{
    uint8_t mods = 0, n = 0, i;
    uint16_t media = 0;
    uint8_t changed = 0;
    uint8_t diff = 0;
    uint8_t *keys = rep_keys;
    uint8_t *mouse = rep_mouse;
    uint8_t *buf = rep_buf;

    keys[0] = keys[1] = keys[2] = keys[3] = keys[4] = keys[5] = 0;
    mouse[0] = mouse[1] = mouse[2] = mouse[3] = 0;

    for (i = 0; i < NUM_KEYS; i++)
        if (active_sw[i]) apply_code(active_sw[i], &mods, keys, &n, &media, mouse);
    if (active_enc_push) apply_code(active_enc_push, &mods, keys, &n, &media, mouse);

    /* 旋钮旋转产生的 2ms 脉冲 */
    if (enc_pulse_cnt) {
        apply_code(enc_pulse, &mods, keys, &n, &media, mouse);
        enc_pulse_cnt--;
        if (enc_pulse_cnt == 0) enc_pulse = 0;
    }

    /* ---- 键盘 ---- */
    buf[0] = REP_KBD;
    buf[1] = mods;
    buf[2] = 0;
    for (i = 0; i < 6; i++) buf[3 + i] = keys[i];

    if (mods != last_kbd[0]) diff = 1;
    for (i = 0; i < 6; i++) if (keys[i] != last_kbd[2 + i]) diff = 1;

    if (diff && usb_ep1_send(buf, 9)) {
        dbg_tx++;
        last_kbd[0] = mods;
        for (i = 0; i < 6; i++) last_kbd[2 + i] = keys[i];
        changed = 1;
    }

    /* ---- 多媒体 ---- */
    if (media != last_media) {
        buf[0] = REP_MEDIA;
        buf[1] = (uint8_t)(media & 0xFF);
        buf[2] = (uint8_t)(media >> 8);
        if (usb_ep1_send(buf, 3)) { last_media = media; dbg_tx++; }
    }

    /* ---- 鼠标 ---- */
    if (mouse[0] != last_mouse[0] || mouse[3] != last_mouse[3]) {
        buf[0] = REP_MOUSE;
        for (i = 0; i < 4; i++) buf[1 + i] = mouse[i];
        if (usb_ep1_send(buf, 5)) {
            for (i = 0; i < 4; i++) last_mouse[i] = mouse[i];
        }
    }

    return changed;
}

/* 内部动作：仅在按下沿执行 */
static void run_action(uint8_t act)
{
    switch (act) {
    case ACT_FN_LAYER: layer_fn = 1; break;
    case ACT_BRI_UP:
        if (cfg.led_bright < 95) cfg.led_bright += 5; else cfg.led_bright = 100;
        break;
    case ACT_BRI_DOWN:
        if (cfg.led_bright > 5) cfg.led_bright -= 5; else cfg.led_bright = 0;
        break;
    case ACT_LED_NEXT:
        cfg.led_mode = (uint8_t)((cfg.led_mode + 1) % 8);
        break;
    /* ACT_RELOAD / ACT_RESET 已删除：只能由配置器经 Feature 通道执行。
       绑到按键上误按一下就会把整张键表清回出厂值，风险太大。 */
    default:
        break;
    }
}

static void keys_update(void)
{
    uint8_t b, k;
    uint16_t code;

    for (b = 0; b < MAT_BITS; b++) {
        uint8_t pressed = (matrix_stable >> b) & 1;
        k = (uint8_t)bit_to_key[b];

        if (b == ENC_BIT) {
            if (pressed && !enc_push_state) {
                enc_push_state = 1;
                code = cfg.enc[2];
                if (code_type(code) == T_ACTION) run_action((uint8_t)(code & 0xFF));
                else if (code_type(code) == T_HOST) host_action_pending = (uint8_t)(code & 0xFF);
                else active_enc_push = code;
            } else if (!pressed && enc_push_state) {
                enc_push_state = 0;
                active_enc_push = 0;
            }

            /* 长按回 BootLoader 已挪到 boot_hold_poll()：那里绕开矩阵状态机，
               即使扫描/去抖/位映射全坏了，自保入口依然有效。 */
            continue;
        }

        if (pressed) {
            if (active_sw[k] == 0) {
                code = cfg.keys[k];
                /* 动作 / 主机动作也必须占住 active_sw，否则"按下沿"判定失效：
                   active_sw[k] 一直为 0，按住不放就等于每 1ms 触发一次
                   （表现为主机动作狂发、亮度一路冲到顶）。 */
                if (code_type(code) == T_ACTION) {
                    run_action((uint8_t)(code & 0xFF));
                    active_sw[k] = code;
                } else if (code_type(code) == T_HOST) {
                    host_action_pending = (uint8_t)(code & 0xFF);
                    active_sw[k] = code;
                } else if (code != 0) {
                    active_sw[k] = code;
                    /* 移动键的按下沿交给 mouse_move_tick 做连击判定 */
                    if (code_type(code) == T_MOVE) move_edge = 1;
                }
            }
        } else {
            active_sw[k] = 0;
        }
    }
}

/* 编码器旋转 → 生成 2ms 的"按键脉冲"（不是连发两次报文，避免 EP1 忙时丢包） */
static void encoder_dispatch(void)
{
    if (enc_delta == 0) return;

    if (enc_delta > 0) {
        enc_pulse = cfg.enc_invert ? cfg.enc[1] : cfg.enc[0];
        enc_net++;
    } else {
        enc_pulse = cfg.enc_invert ? cfg.enc[0] : cfg.enc[1];
        enc_net--;
    }
    enc_delta = 0;
    enc_pulse_cnt = 2;

    if (code_type(enc_pulse) == T_ACTION) {
        run_action((uint8_t)(enc_pulse & 0xFF));
        enc_pulse_cnt = 0;
        enc_pulse = 0;
    } else if (code_type(enc_pulse) == T_HOST) {
        host_action_pending = (uint8_t)(enc_pulse & 0xFF);
        enc_pulse_cnt = 0;
        enc_pulse = 0;
    }
}

/* ============================ WS2812 ============================ */
/*
 * 时序（WS2812B，24MHz，1 周期 = 41.67ns）：
 *   '1' 高 ~0.75us(18 周期) / '0' 高 ~0.29us(7 周期)，位周期 ~1.25us(30 周期)
 *
 * !! 这里用汇编是因为 C 生成的代码时序不可控。烧录后必须用逻辑分析仪
 *    抓 P3.3 验证：'1' 的高电平应在 0.65~0.95us，'0' 应在 0.25~0.45us。
 *    偏差方向：高电平偏短 → 增加对应的 nop；偏长 → 减少 nop。
 *    下面的 18/7 与 10/20 是按 E8051(1T) 指令周期估算的初值。
 */
__data uint8_t ws_tmp;
static __data uint8_t ws_i;   /* 刷新循环的下标，放 __data 避免被寄存器冲掉 */

#ifndef NO_LED_OUTPUT          /* NO_LED_OUTPUT 构建下整段不编译，避免死代码告警 */
#ifdef HOST_CHECK
/* 主机端语法检查时跳过汇编实现 */
static void ws_send_byte(void) { }
#else
static void ws_send_byte(void) __naked
{
    __asm
        ; NOTE: keep comments ASCII-only here. SDCC re-emits non-ASCII
        ; characters as \U0000xxxx escapes and sdas8051 then fails silently.
        ;
        ; Save the caller R6/R7 registers. This is a __naked function, so SDCC emits no
        ; prologue. ws2812_flush() keeps its loop index in R7, and this
        ; routine uses R7 as the bit counter (8..1) -> the index was being
        ; destroyed, the loop never terminated and EA stayed disabled, which
        ; killed both the main loop and USB.
        push 0x06
        push 0x07
        mov  a, _ws_tmp
        mov  r6, a
        mov  r7, #8
    00001$:
        setb 0xB3              ; P3.3 = 1
        mov  a, r6
        rlc  a
        mov  r6, a
        jc   00002$
        nop
        clr  0xB3
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        djnz r7, 00001$
        pop  0x07
        pop  0x06
        ret
    00002$:
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        clr  0xB3
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        djnz r7, 00001$
        pop  0x07
        pop  0x06
        ret
    __endasm;
}
#endif /* HOST_CHECK */
#endif /* NO_LED_OUTPUT */

static void ws2812_flush(void)
{
#if defined(NO_LED_OUTPUT) || defined(DIAG_MARK)
    /* 不整条刷新。NO_LED_OUTPUT：完全不点灯（安全默认）。
       DIAG_MARK：只允许 led_mark() 点亮前两颗当里程碑指示灯。 */
    LED_PIN = 0;
    return;
#else
    EA_DISABLE();
    for (ws_i = 0; ws_i < 3 * NUM_LEDS; ws_i++) {
        ws_tmp = led_buf[ws_i];
        ws_send_byte();
    }
    LED_PIN = 0;
    EA_ENABLE();
#endif
}

#ifdef DIAG_MARK
/* 里程碑指示：点亮前 n 颗为红色（最多 2 颗，≤120mA）
   n=1 → 已经走完"USB 枚举等待"
   n=2 → 已经走完 app_init，主循环开始运行 */
static void led_mark(uint8_t n)
{
    uint8_t i;
    EA_DISABLE();
    for (i = 0; i < 2; i++) {
        ws_tmp = 0x00;                          /* G */
        ws_send_byte();
        ws_tmp = (i < n) ? 0xFF : 0x00;         /* R */
        ws_send_byte();
        ws_tmp = 0x00;                          /* B */
        ws_send_byte();
    }
    LED_PIN = 0;
    EA_ENABLE();
}
#endif

/* ============================ 灯效 ============================ */
static void hue2rgb(uint8_t h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = (uint8_t)(h / 43);
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t q = (uint8_t)(255 - rem);

    switch (region) {
    case 0: *r = 255; *g = rem; *b = 0;   break;
    case 1: *r = q;   *g = 255; *b = 0;   break;
    case 2: *r = 0;   *g = 255; *b = rem; break;
    case 3: *r = 0;   *g = q;   *b = 255; break;
    case 4: *r = rem; *g = 0;   *b = 255; break;
    default:*r = 255; *g = 0;   *b = q;   break;
    }
}

/* ---- 灯珠串联顺序 ←→ 键索引（2026-11-11 按实物串接顺序标定） ----
 *
 * 实物灯链：CN11 → CN10 → CN9 → CN5 → CN6 → CN7 → CN8 → CN4 → CN3 → CN2 → CN1
 * （用户实测，优先于早先按 PCB 坐标反推的结果）
 *
 * 键索引 = CN 编号 - 1，于是"链条第 i 颗 → 键索引"就是下面这张表。
 * per_key[] 按**链条序号**存，所以常亮/呼吸直接用 per_key[i]，
 * 只有"这颗灯属于哪个键"的模式（按压缩放）才查 LED_KEY / LED_KEYBIT。
 */
static __code const uint8_t LED_KEY[NUM_LEDS] = { 10, 9, 8, 4, 5, 6, 7, 3, 2, 1, 0 };
static __code const uint8_t LED_KEYBIT[NUM_LEDS] = { 2, 1, 0, 4, 5, 6, 7, 11, 10, 9, 8 };

/* 满亮度上限：把原来 100% 的 70% 定为现在的 100%（整体压暗） */
#define LED_FULL  70

static void led_effect(void)
{
    uint8_t i, r, g, b;
    uint8_t scan_head = (uint8_t)((led_t / 2) % NUM_LEDS);   /* 扫描/波浪用 */
    uint8_t t8 = (uint8_t)(led_t >> 2);                      /* 慢速伪随机用 */
    uint16_t base = (uint16_t)((uint16_t)cfg.led_bright * LED_FULL / 100);

    if (cfg.led_mode == 4 || cfg.led_bright == 0) {
        for (i = 0; i < 3 * NUM_LEDS; i++) led_buf[i] = 0;
        return;
    }

    for (i = 0; i < NUM_LEDS; i++) {
        uint16_t mul = base;                            /* 0..LED_FULL */

        switch (cfg.led_mode) {
        case 1: {                                       /* 呼吸 */
            int16_t s = (int16_t)(led_t + i * 6);
            int16_t v = (int16_t)((s % 120) - 60);
            if (v < 0) v = -v;
            mul = (uint16_t)((base * (uint16_t)(100 - v * 3 / 2)) / 100);
            r = cfg.per_key[i][0]; g = cfg.per_key[i][1]; b = cfg.per_key[i][2];
            break;
        }
        case 2:                                         /* 彩虹 */
            hue2rgb((uint8_t)((i * 23) + (led_t * 2)), &r, &g, &b);
            break;
        case 3:                                         /* 按压缩放 */
            mul = (matrix_stable & (uint16_t)(1u << LED_KEYBIT[i]))
                  ? base : (uint16_t)(base / 5);
            r = cfg.per_key[i][0]; g = cfg.per_key[i][1]; b = cfg.per_key[i][2];
            break;
        case 5: {                                       /* 彩虹呼吸 */
            int16_t s2 = (int16_t)(led_t % 120);
            int16_t v2 = (int16_t)(s2 - 60);
            if (v2 < 0) v2 = -v2;
            hue2rgb((uint8_t)((i * 23) + (led_t * 2)), &r, &g, &b);
            mul = (uint16_t)((base * (uint16_t)(100 - v2 * 3 / 2)) / 100);
            break;
        }
        case 6:                                         /* 单点扫描（带拖尾） */
            if (i == scan_head) { r = cfg.led_rgb[0]; g = cfg.led_rgb[1]; b = cfg.led_rgb[2]; }
            else if (i == (uint8_t)((scan_head + NUM_LEDS - 1) % NUM_LEDS)) {
                r = (uint8_t)(cfg.led_rgb[0] / 3);
                g = (uint8_t)(cfg.led_rgb[1] / 3);
                b = (uint8_t)(cfg.led_rgb[2] / 3);
            } else { r = 0; g = 0; b = 0; }
            mul = base;
            break;
        case 7: {                                       /* 星光闪烁 */
            uint8_t x = (uint8_t)(i * 41 + t8 * 17);
            x = (uint8_t)((x << 1) ^ (x >> 3));
            if (x > 190)      { r = cfg.led_rgb[0]; g = cfg.led_rgb[1]; b = cfg.led_rgb[2]; }
            else if (x > 140) { r = (uint8_t)(cfg.led_rgb[0] / 2);
                                g = (uint8_t)(cfg.led_rgb[1] / 2);
                                b = (uint8_t)(cfg.led_rgb[2] / 2); }
            else              { r = 0; g = 0; b = 0; }
            mul = base;
            break;
        }
        default:                                        /* 常亮 */
            r = cfg.per_key[i][0]; g = cfg.per_key[i][1]; b = cfg.per_key[i][2];
            break;
        }

        led_buf[i * 3 + 0] = (uint8_t)((uint16_t)g * mul / 100);
        led_buf[i * 3 + 1] = (uint8_t)((uint16_t)r * mul / 100);
        led_buf[i * 3 + 2] = (uint8_t)((uint16_t)b * mul / 100);
    }

    /* ---- 电流限幅：估算超过 400mA 时整体等比压暗 ----
       sum = 11 颗灯珠的 R+G+B 之和（满值 11*3*255 = 8415）
       电流估算：11mA（主控）+ 19mA * sum/255，400mA 对应 sum ≈ 5200 */
    {
        uint16_t sum = 0;
        uint8_t k;
        for (k = 0; k < 3 * NUM_LEDS; k++) sum += led_buf[k];
        if (sum > LED_SUM_LIMIT) {
            uint8_t sh = 1;
            while (sh < 4 && (sum >> sh) > LED_SUM_LIMIT) sh++;
            for (k = 0; k < 3 * NUM_LEDS; k++) led_buf[k] >>= sh;
        }
    }
}

/* ============================ Feature 协议 ============================ */
#define CMD_INFO        0x01
#define CMD_GET_KEYMAP  0x02
#define CMD_SET_KEY     0x03
#define CMD_SET_ALL     0x04
#define CMD_GET_ENC     0x05
#define CMD_SET_ENC     0x06
#define CMD_GET_LED     0x07
#define CMD_SET_LED     0x08
#define CMD_SAVE        0x09
#define CMD_FACTORY     0x0A
#define CMD_BOOTLOADER  0x0B
#define CMD_DEBUG       0x20      /* 诊断遥测：矩阵 / 编码器 / 主循环计数 */
#define CMD_RELOAD      0x21      /* 从 DataFlash 重新加载配置（验证是否真的写进去） */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

void app_feature(const uint8_t *in, uint8_t *out)
{
    uint8_t cmd = in[0];
    uint8_t i;

    /* 63 = Feature 负载长度（报告 ID 0x10 + 63 = EP0 单包 64 字节）。
       所有命令的输入输出都远小于 63，见 usb.c 文件头注释。 */
    for (i = 0; i < 63; i++) out[i] = 0;
    out[0] = (uint8_t)(cmd | 0x80);
    out[1] = 0;                                  /* 0 = OK */

    switch (cmd) {
    case CMD_INFO:
        out[1] = 0;
        out[2] = PROTO_VERSION;
        out[3] = FW_VERSION_H;
        out[4] = FW_VERSION_L;
        out[5] = MAT_ROWS;
        out[6] = MAT_COLS;
        out[7] = NUM_KEYS;
        out[8] = NUM_LEDS;
        out[9] = 0;                              /* 保留 */
        for (i = 0; i < 16 && DevName[i]; i++) out[10 + i] = (uint8_t)DevName[i];
        break;

    case CMD_GET_KEYMAP:
        for (i = 0; i < NUM_KEYS; i++) put16(&out[2 + i * 2], cfg.keys[i]);
        break;

    case CMD_SET_KEY:
        if (in[1] >= NUM_KEYS) { out[1] = 2; break; }
        cfg.keys[in[1]] = get16(&in[2]);
        break;

    case CMD_SET_ALL:
        for (i = 0; i < NUM_KEYS; i++) cfg.keys[i] = get16(&in[1 + i * 2]);
        break;

    case CMD_GET_ENC:
        put16(&out[2], cfg.enc[0]);
        put16(&out[4], cfg.enc[1]);
        put16(&out[6], cfg.enc[2]);
        out[8] = cfg.enc_invert;
        break;

    case CMD_SET_ENC:
        cfg.enc[0] = get16(&in[1]);
        cfg.enc[1] = get16(&in[3]);
        cfg.enc[2] = get16(&in[5]);
        cfg.enc_invert = in[7] ? 1 : 0;
        break;

    case CMD_GET_LED:
        out[2] = cfg.led_mode;
        out[3] = cfg.led_bright;
        out[4] = cfg.led_speed;
        out[5] = cfg.led_rgb[0];
        out[6] = cfg.led_rgb[1];
        out[7] = cfg.led_rgb[2];
        for (i = 0; i < NUM_LEDS; i++) {
            out[8 + i * 3 + 0] = cfg.per_key[i][0];
            out[8 + i * 3 + 1] = cfg.per_key[i][1];
            out[8 + i * 3 + 2] = cfg.per_key[i][2];
        }
        break;

    case CMD_SET_LED:
        cfg.led_mode = in[1];
        cfg.led_bright = in[2];
        cfg.led_speed = in[3];
        cfg.led_rgb[0] = in[4];
        cfg.led_rgb[1] = in[5];
        cfg.led_rgb[2] = in[6];
        for (i = 0; i < NUM_LEDS; i++) {
            cfg.per_key[i][0] = in[7 + i * 3 + 0];
            cfg.per_key[i][1] = in[7 + i * 3 + 1];
            cfg.per_key[i][2] = in[7 + i * 3 + 2];
        }
        break;

    case CMD_SAVE:
        save_pending = 1;                        /* 真正写 Flash 在主循环做 */
        break;

    case CMD_FACTORY:
        cfg_defaults();
        save_pending = 1;
        break;

    case CMD_BOOTLOADER:
        break;                                   /* CH552 需冷启动才能进 ISP，这里只做占位 */

    case CMD_RELOAD:
        /* 重新从 DataFlash 读一遍。用来验证「烧录的配置是否真的落盘」，
           不需要拔插电源。如果读出来和刚写的不一样，就是 Flash 写入失败。 */
        cfg_load();
        break;

    case CMD_DEBUG:
        /* 让配置器 / 命令行工具能"看见"芯片内部状态，不用再靠猜。
           [2..3] matrix_raw   [4..5] matrix_stable
           [6] 主循环计数      [7] EP1 成功发送次数
           [8] 编码器原始 AB   [9] 待上报刻度
           [10] 长按计数低字节 [11] P1 [12] P3 */
        out[2] = (uint8_t)(matrix_raw & 0xFF);
        out[3] = (uint8_t)(matrix_raw >> 8);
        out[4] = (uint8_t)(matrix_stable & 0xFF);
        out[5] = (uint8_t)(matrix_stable >> 8);
        out[6] = dbg_ticks;
        out[7] = dbg_tx;
        out[8] = enc_prev;
        out[9] = (uint8_t)enc_net;
        out[10] = (uint8_t)(boot_ticks & 0xFF);
        out[11] = (uint8_t)P1;
        out[12] = (uint8_t)P3;
        break;

    default:
        out[1] = 1;                              /* 未知命令 */
        break;
    }
}

/* ============================ 状态报文 ============================ */
static void send_status(void)
{
    uint8_t buf[9];
    buf[0] = REP_STATUS;
    buf[1] = (uint8_t)(matrix_stable & 0xFF);
    buf[2] = (uint8_t)((matrix_stable >> 8) & 0x0F);
    buf[3] = (uint8_t)enc_net;
    buf[4] = (uint8_t)(layer_fn ? 1 : 0);
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = 0;
    if (usb_ep1_send(buf, 9)) enc_net = 0;
}

/* ============================ 初始化 ============================ */
static void clock_init(void)
{
    /* Fsys = 24MHz（VCC >= 4.5V 才允许），Fpll = 96MHz，USB = 48MHz */
    safe_mode_enter();
    CLOCK_CFG = 0x86;          /* bOSC_EN_INT=1, MASK_SYS_CK_SEL=110 → 24MHz */
}

void app_init(void)
{
    uint8_t i;

    gpio_init();
    cfg_load();

    enc_prev = (uint8_t)((ENC_A_PIN ? 2 : 0) | (ENC_B_PIN ? 1 : 0));
    enc_acc = 0;
    enc_delta = 0;
    enc_net = 0;
    enc_pulse = 0;
    enc_pulse_cnt = 0;
    enc_push_state = 0;
    layer_fn = 0;
    save_pending = 0;
    matrix_stable = 0;
    matrix_raw = 0;
    for (i = 0; i < MAT_BITS; i++) debounce_cnt[i] = 0;
    for (i = 0; i < NUM_KEYS; i++) active_sw[i] = 0;
    for (i = 0; i < 8; i++) last_kbd[i] = 0;
    for (i = 0; i < 4; i++) last_mouse[i] = 0;
    last_media = 0;

    led_effect();
    ws2812_flush();

    timer0_init();
    EA = 1;
}

/* ============================ 1ms 节拍 ============================ */
void app_tick(void)
{
    static uint8_t div20;

    dbg_ticks++;
    matrix_scan();
    boot_hold_poll();      /* 自保入口：直接读旋钮按压，绕开矩阵状态机 */
    matrix_debounce();
    encoder_poll();
    keys_update();
    encoder_dispatch();
    mouse_move_tick();
    build_reports();

    /* 主机动作触发报文（ID 5），只发一次。
       注意判 HOST_ACTION_NONE 而不是判 0：动作编号 0 是合法槽位。 */
    if (host_action_pending != HOST_ACTION_NONE) {
        uint8_t hb[2];
        hb[0] = REP_HOST;
        hb[1] = host_action_pending;
        if (usb_ep1_send(hb, 2)) host_action_pending = HOST_ACTION_NONE;
    }

    if (++div20 >= 20) {                    /* 20ms 一拍 */
        static uint16_t led_acc;
        div20 = 0;
        /* 速度：led_speed 0..100 → 每拍推进 0.25～2 个 led_t（4x～1/2 速）
           原来 led_speed 只存不用，滑杆怎么拉都没反应，这里接上。 */
        led_acc = (uint16_t)(led_acc + 25u + (uint16_t)cfg.led_speed * 7u / 4u);
        while (led_acc >= 100u) {
            led_acc = (uint16_t)(led_acc - 100u);
            led_t++;
        }
        led_effect();
        ws2812_flush();
        send_status();
    }

    if (save_pending && !matrix_stable) {
        save_pending = 0;
        cfg_save();
    }
}

/* ============================ 入口 ============================ */
void main(void)
{
    clock_init();       /* 24MHz：必须在 USB 之前，且之后不要再改 */
    usb_init();         /* USB 先拉起来 */

    /* 枚举窗口：只做 USB 轮询，最多约 2 秒。
       目的是让 USB 在任何业务初始化之前就完成枚举，
       这样后面 app_init 里就算有东西卡住，也不会连累 USB。 */
    {
        uint16_t spin = 0;
        while (!usb_configured() && spin < 50000) {
            usb_poll();
            spin++;
        }
    }

#ifdef DIAG_MARK
    led_mark(1);        /* 里程碑 1：枚举等待已结束 */
#endif

    app_init();         /* 矩阵 / 编码器 / 灯效 / 配置 */

#ifdef DIAG_MARK
    led_mark(2);        /* 里程碑 2：app_init 已完成，主循环即将开始 */
#endif

    for (;;) {
        usb_poll();                 /* USB 没有中断向量，必须高频轮询 */
        if (tick_flag) {
            tick_flag = 0;
            app_tick();
        }
    }
}
