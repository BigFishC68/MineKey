/* =========================================================================
 * usb.c —— CH552 USB 全速设备 + 复合 HID
 *
 * 端点分配：
 *   EP0  控制传输（含 HID Feature Report 0x10，63 字节负载）
 *   EP1  IN 中断，16 字节/1ms，承载 ID1(键盘) / ID2(媒体) / ID3(状态) / ID4(鼠标)
 *
 * 重要：bMaxPacketSize0 = 64，Feature 报文故意设计成「ID + 63 负载 = 64 字节」，
 *       一个 EP0 包就能收发完。绝不能把负载加到 64：那会让控制传输分两包，
 *       而下面的 OUT 处理只认第一个包，第 64 字节负载会被丢弃，且提前 arm
 *       状态阶段会让某些主机的控制传输错乱（HidD_SetFeature 直接报失败）。
 *       （CH552 的 EP0 缓冲区正好 64 字节。）
 * ========================================================================= */
#include "board.h"
#include "usb.h"
#include "app.h"

/* ---------------- 端点缓冲区（放在 xRAM 顶端，见 Makefile --xram-size） -------- */
__xdata __at (0x0380) uint8_t Ep0Buffer[64];
__xdata __at (0x03C0) uint8_t Ep1Buffer[64];

/* ---------------- 设备描述符 ---------------- */
__code const uint8_t DevDesc[] = {
    0x12, 0x01,             /* bLength, DEVICE */
    0x00, 0x02,             /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,       /* class/subclass/protocol（由接口定义） */
    0x40,                   /* bMaxPacketSize0 = 64 */
    USB_VID & 0xFF, USB_VID >> 8,
    USB_PID & 0xFF, USB_PID >> 8,
    0x00, 0x01,             /* bcdDevice 1.00 */
    0x01, 0x02, 0x03,       /* iManufacturer / iProduct / iSerial */
    0x01                    /* bNumConfigurations */
};

__code const uint8_t CfgDesc[] = {
    /* 配置描述符 */
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    /* 接口描述符：HID，无引导子类 */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    /* HID 描述符：HID 1.11，报告描述符 157 字节 */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0xAC, 0x00,
    /* 端点描述符：EP1 IN 中断，16 字节，1ms */
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x01
};

/* ---------------- 报告描述符（172 字节，与 CfgDesc 里的 0xAC 一致） ---------- */
__code const uint8_t ReportDesc[] = {
    /* --- 键盘，Report ID 1，8 字节 --- */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x06,             /* Usage (Keyboard) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    0x05, 0x07,             /*   Usage Page (Keyboard) */
    0x19, 0xE0,             /*   Usage Min (0xE0) */
    0x29, 0xE7,             /*   Usage Max (0xE7) */
    0x15, 0x00,             /*   Logical Min (0) */
    0x25, 0x01,             /*   Logical Max (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x08,             /*   Report Count (8) */
    0x81, 0x02,             /*   Input (Data,Var,Abs) 修饰键 */
    0x95, 0x01,             /*   Report Count (1) */
    0x75, 0x08,             /*   Report Size (8) */
    0x81, 0x01,             /*   Input (Const) 保留字节 */
    0x95, 0x06,             /*   Report Count (6) */
    0x75, 0x08,             /*   Report Size (8) */
    0x15, 0x00,             /*   Logical Min (0) */
    0x25, 0x65,             /*   Logical Max (0x65) */
    0x05, 0x07,             /*   Usage Page (Keyboard) */
    0x19, 0x00,             /*   Usage Min (0) */
    0x29, 0x65,             /*   Usage Max (0x65) */
    0x81, 0x00,             /*   Input (Data,Array) 6 键无冲 */
    0xC0,                   /* End Collection */

    /* --- 多媒体，Report ID 2，2 字节 --- */
    0x05, 0x0C,             /* Usage Page (Consumer) */
    0x09, 0x01,             /* Usage (Consumer Control) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x02,             /*   Report ID (2) */
    0x15, 0x00,             /*   Logical Min (0) */
    0x26, 0xFF, 0x03,       /*   Logical Max (0x3FF) */
    0x19, 0x00,             /*   Usage Min (0) */
    0x2A, 0xFF, 0x03,       /*   Usage Max (0x3FF) */
    0x75, 0x10,             /*   Report Size (16) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x00,             /*   Input (Data,Array) */
    0xC0,                   /* End Collection */

    /* --- 鼠标，Report ID 4，4 字节（只是按钮 + 滚轮） --- */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x04,             /*   Report ID (4) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Min (1) */
    0x29, 0x03,             /*     Usage Max (3) */
    0x15, 0x00,             /*     Logical Min (0) */
    0x25, 0x01,             /*     Logical Max (1) */
    0x95, 0x03,             /*     Report Count (3) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x05,             /*     Report Size (5) 补齐 */
    0x81, 0x01,             /*     Input (Const) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x09, 0x38,             /*     Usage (Wheel) */
    0x15, 0x81,             /*     Logical Min (-127) */
    0x25, 0x7F,             /*     Logical Max (127) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x03,             /*     Report Count (3) */
    0x81, 0x06,             /*     Input (Data,Var,Rel) */
    0xC0,                   /*   End Collection (Physical) */
    0xC0,                   /* End Collection (Application) */

    /* --- 厂商自定义，Report ID 3（状态，输入）+ 0x10（配置，Feature） --- */
    0x06, 0x00, 0xFF,       /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,             /* Usage (0x01) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x03,             /*   Report ID (3) */
    0x15, 0x00,             /*   Logical Min (0) */
    0x26, 0xFF, 0x00,       /*   Logical Max (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x08,             /*   Report Count (8) */
    0x09, 0x01,             /*   Usage (0x01) */
    0x81, 0x02,             /*   Input (Data,Var,Abs) */
    0x85, 0x05,             /*   Report ID (5) 主机动作触发 */
    0x15, 0x00,             /*   Logical Min (0) */
    0x26, 0x3F, 0x00,       /*   Logical Max (63) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0x09, 0x03,             /*   Usage (0x03) */
    0x81, 0x02,             /*   Input (Data,Var,Abs) */
    0x85, 0x10,             /*   Report ID (0x10) */
    0x95, 0x3F,             /*   Report Count (63) 负载 63 字节：
                               63 + 报告 ID = 64 = EP0 单包上限，见文件头注释 */
    0x09, 0x02,             /*   Usage (0x02) */
    0xB1, 0x02,             /*   Feature (Data,Var,Abs) */
    0xC0                    /* End Collection */
};

/* 编译期校验描述符长度（不依赖 C11 的 _Static_assert） */
typedef char assert_report_desc_len[(sizeof(ReportDesc) == 172) ? 1 : -1];
typedef char assert_cfg_desc_len[(sizeof(CfgDesc) == 34) ? 1 : -1];

/* ---------------- 字符串描述符 ---------------- */
static __code const char StrManu[] = "KeyForge";
static __code const char StrProd[] = "KeyForge 11-Key Pad";
static __code const char StrSer[]  = "KF00000001";

/* ---------------- EP0 状态 ----------------
   注意：这些变量全部放 xRAM。CH552 内部 RAM 只有 256 字节，还要留栈，
   DSEG 一旦超了就会链接失败（Could not get N consecutive bytes）。 */
static __code const uint8_t *ep0_ptr;
static __xdata uint16_t ep0_remain;
static __xdata uint8_t  ep0_set_report;   /* 1 = 下一个 OUT 是 SET_REPORT 数据 */
static __xdata uint8_t  ep0_set_off;      /* SET_REPORT 负载在 Ep0Buffer 里的偏移 */
static __xdata uint8_t  ep0_pending_addr;
static __xdata uint8_t  ep0_has_addr;
static __xdata uint8_t  usb_cfg;          /* 已配置的配置号 */
static __xdata volatile uint8_t ep1_busy; /* EP1 上一包是否还在发送 */

/* Feature Report 暂存（配置器读写用）
   总线上是 64 字节：FeatureBuf[0] = 报告 ID (0x10)，负载在 [1..63]。
   app_feature() 只看到负载部分（&FeatureBuf[1]）。 */
__xdata uint8_t FeatureBuf[64];

/* =========================================================================
 * 工具
 * ========================================================================= */
/* -------------------------------------------------------------------------
 * 控制传输的数据翻转（DATA0 / DATA1）
 *
 * CH552 的 EP0 是**手动翻转**模式：bUEP_R_TOG / bUEP_T_TOG 必须由软件维护。
 * 控制读的第一个数据包必须是 DATA1，之后每包翻转一次。
 *
 * 我们原来从不碰这两位 —— 描述符永远以 DATA0 发出，而主机在控制传输里
 * 期待 DATA1，于是包被直接丢弃、主机超时，报「设备描述符请求失败」。
 * 这就是「D+ 有 3V、芯片在跑，但主机认不到」的真正原因。
 *
 * 下面两段对应官方例程 / wagiminator CH552 实现的
 * USB_EP0_SETUP() 尾部 + USB_EP0_IN()。
 * ------------------------------------------------------------------------- */
static void ep0_arm_in(uint8_t len)      /* 第一个数据包：显式置 DATA1 */
{
    UEP0_T_LEN = len;
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
}

static void ep0_next_in(uint8_t len)     /* 后续数据包：翻到下一相 */
{
    UEP0_T_LEN = len;
    UEP0_CTRL ^= bUEP_T_TOG;
}

static void ep0_stall(void)
{
    /* 与官方例程一致：两个方向都 STALL，并把翻转位归一，
       下一次 SETUP 重新从 DATA1 开始。 */
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
}

/* 从 code 空间发送，自动分包 */
static void ep0_send_code(__code const uint8_t *p, uint16_t len)
{
    uint8_t n = (len > 64) ? 64 : (uint8_t)len;
    uint8_t i;
    for (i = 0; i < n; i++) Ep0Buffer[i] = p[i];
    ep0_ptr = p + n;
    ep0_remain = len - n;
    ep0_arm_in(n);
}

/* xRAM 数据在首包之后的尾巴（兜底逻辑：正常不会触发——FeatureBuf 连 ID 共
   64 字节，恰好一个 EP0 包；留着这段是为了万一将来报文加长时不至于断流）。 */
static __xdata uint8_t ep0_off;      /* 尾巴数据在 FeatureBuf 里的下标 */
static __xdata uint8_t ep0_tail;     /* 尾巴还有多少字节 */

/* 从 xRAM 发送。len 可以超过 64：首包 64 字节，余下由 IN 处理补发。 */
static void ep0_send_ram(const uint8_t *p, uint16_t len)
{
    uint8_t n = (len > 64) ? 64 : (uint8_t)len;
    uint8_t i;
    for (i = 0; i < n; i++) Ep0Buffer[i] = p[i];
    ep0_remain = 0;
    ep0_off = n;
    ep0_tail = (uint8_t)(len - n);
    ep0_arm_in(n);
}

/* 字符串描述符：UTF-16LE */
static void ep0_send_string(uint8_t idx, uint16_t maxlen)
{
    __code const char *s;
    uint8_t len;
    uint8_t i;
    uint8_t n;

    if (idx == 0) {                       /* 语言 ID */
        Ep0Buffer[0] = 4; Ep0Buffer[1] = 0x03;
        Ep0Buffer[2] = 0x09; Ep0Buffer[3] = 0x04;
        ep0_remain = 0;
        ep0_arm_in((maxlen < 4) ? (uint8_t)maxlen : 4);
        return;
    }
    if (idx == 1) s = StrManu;
    else if (idx == 2) s = StrProd;
    else if (idx == 3) s = StrSer;
    else { ep0_stall(); return; }

    len = 0;
    while (s[len] && len < 28) len++;
    n = (uint8_t)(2 + len * 2);
    Ep0Buffer[0] = n;
    Ep0Buffer[1] = 0x03;
    for (i = 0; i < len; i++) {
        Ep0Buffer[2 + i * 2] = (uint8_t)s[i];
        Ep0Buffer[3 + i * 2] = 0;
    }
    ep0_remain = 0;
    ep0_arm_in((maxlen < n) ? (uint8_t)maxlen : n);
}

/* =========================================================================
 * 标准 / 类请求
 * ========================================================================= */
static void ep0_setup(void)
{
    uint8_t  bmRT   = Ep0Buffer[0];
    uint8_t  bReq   = Ep0Buffer[1];
    uint16_t wValue = Ep0Buffer[2] | ((uint16_t)Ep0Buffer[3] << 8);
    uint16_t wLength = Ep0Buffer[6] | ((uint16_t)Ep0Buffer[7] << 8);

    ep0_remain = 0;
    ep0_tail = 0;
    ep0_set_report = 0;

    /* ---- 标准请求 ---- */
    if ((bmRT & 0x60) == 0x00) {
        switch (bReq) {
        case 0x06:                                  /* GET_DESCRIPTOR */
            switch (wValue >> 8) {
            case 0x01:
                ep0_send_code(DevDesc, (wLength < sizeof(DevDesc)) ? wLength : sizeof(DevDesc));
                break;
            case 0x02:
                ep0_send_code(CfgDesc, (wLength < sizeof(CfgDesc)) ? wLength : sizeof(CfgDesc));
                break;
            case 0x03:
                ep0_send_string(wValue & 0xFF, wLength);
                break;
            case 0x22:
                ep0_send_code(ReportDesc, (wLength < sizeof(ReportDesc)) ? wLength : sizeof(ReportDesc));
                break;
            default:
                ep0_stall();
                break;
            }
            break;

        case 0x05:                                  /* SET_ADDRESS */
            ep0_pending_addr = wValue & 0x7F;
            ep0_has_addr = 1;
            UEP0_T_LEN = 0;
            ep0_arm_in(0);
            break;

        case 0x09:                                  /* SET_CONFIGURATION */
            usb_cfg = (uint8_t)wValue;
            UEP0_T_LEN = 0;
            ep0_arm_in(0);
            break;

        case 0x08:                                  /* GET_CONFIGURATION */
            Ep0Buffer[0] = usb_cfg;
            ep0_arm_in(1);
            break;

        case 0x00:                                  /* GET_STATUS */
            Ep0Buffer[0] = 0; Ep0Buffer[1] = 0;
            ep0_arm_in(2);
            break;

        case 0x0A:                                  /* GET_INTERFACE */
            Ep0Buffer[0] = 0;
            ep0_arm_in(1);
            break;

        case 0x0B:                                  /* SET_INTERFACE */
            UEP0_T_LEN = 0;
            ep0_arm_in(0);
            break;

        default:
            ep0_stall();
            break;
        }
        return;
    }

    /* ---- HID 类请求 ---- */
    if ((bmRT & 0x60) == 0x20) {
        switch (bReq) {
        case 0x0A:                                  /* SET_IDLE */
        case 0x0B:                                  /* SET_PROTOCOL */
            UEP0_T_LEN = 0;
            ep0_arm_in(0);
            break;

        case 0x02:                                  /* GET_IDLE */
            Ep0Buffer[0] = 0;
            ep0_arm_in(1);
            break;

        case 0x03:                                  /* GET_PROTOCOL */
            Ep0Buffer[0] = 1;
            ep0_arm_in(1);
            break;

        case 0x01:                                  /* GET_REPORT */
            /* HID 规范：报告描述符里带 Report ID 时，控制传输的数据里
               **第一个字节就是 Report ID**，所以 Feature 报告在总线上是
               64 字节（0x10 + 63 字节负载），EP0 单包发完，不会分页。 */
            if (((wValue >> 8) & 0xFF) == 0x03 && (wValue & 0xFF) == 0x10) {
                ep0_send_ram(FeatureBuf, (wLength < 64) ? wLength : 64);
            } else {
                ep0_stall();
            }
            break;

        case 0x09:                                  /* SET_REPORT */
            if (((wValue >> 8) & 0xFF) == 0x03 && (wValue & 0xFF) == 0x10 && wLength) {
                /* 带报告 ID 时 wLength = 64（ID + 63 负载），负载从第 2 个字节开始 */
                ep0_set_off = (wLength >= 64) ? 1 : 0;
                ep0_set_report = 1;
                UEP0_CTRL = (UEP0_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
            } else {
                UEP0_T_LEN = 0;
                ep0_arm_in(0);
            }
            break;

        default:
            ep0_stall();
            break;
        }
        return;
    }

    ep0_stall();
}

/* =========================================================================
 * 端点初始化
 * ========================================================================= */
static void ep_init(void)
{
    SET_UEP0_DMA(Ep0Buffer);
    SET_UEP1_DMA(Ep1Buffer);

    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    UEP0_T_LEN = 0;

    UEP4_1_MOD = 0x40;                 /* bUEP1_TX_EN = 1，只开 IN */
    UEP2_3_MOD = 0x00;
    UEP1_CTRL = UEP_T_RES_NAK | bUEP_AUTO_TOG;
    UEP1_T_LEN = 0;

    ep1_busy = 0;
    ep0_set_report = 0;
    ep0_remain = 0;
    usb_cfg = 0;
    ep0_has_addr = 0;
}

/* =========================================================================
 * USB 中断
 * ========================================================================= */
/* =========================================================================
 * USB 事件处理（轮询，不是中断）
 *
 * 为什么不写成 __interrupt(8)：
 *   SDCC 的 mcs51 后端只给中断号 0~5 生成跳转向量（0x03..0x2B）。
 *   CH552 的 USB 中断号是 8，向量在 0x0043，SDCC 会**静默不生成**，
 *   结果是 USB 事件永远进不来、而且若置位 IE_EX_USB 会让 CPU 跳到
 *   0x0043 处的普通代码上跑飞。
 *   所以改为主循环高频调用本函数，同时顺带消除 ISR 与主循环并发
 *   访问 cfg / ep1_busy 的竞态。
 * ========================================================================= */
void usb_poll(void)
{
    uint8_t fg = USB_INT_FG;
    uint8_t st;
    uint8_t ep;
    uint8_t token;
    uint8_t n;

    if (fg & UIF_BUS_RST) {
        usb_cfg = 0;
        ep0_has_addr = 0;
        USB_DEV_AD = 0x00;
        ep_init();
    }

    if (fg & UIF_TRANSFER) {
        st    = USB_INT_ST;
        ep    = st & MASK_UIS_ENDP;
        token = st & MASK_UIS_TOKEN;

        if (ep == 0) {
            if (token == UIS_TOKEN_SETUP) {
                ep0_setup();
            } else if (token == UIS_TOKEN_IN) {
                if (ep0_has_addr) {                    /* 地址在状态阶段之后生效 */
                    USB_DEV_AD = (USB_DEV_AD & 0x80) | ep0_pending_addr;
                    ep0_has_addr = 0;
                }
                if (ep0_remain) {
                    n = (ep0_remain > 64) ? 64 : (uint8_t)ep0_remain;
                    {
                        uint8_t i;
                        for (i = 0; i < n; i++) Ep0Buffer[i] = ep0_ptr[i];
                    }
                    ep0_ptr += n;
                    ep0_remain -= n;
                    ep0_next_in(n);
                } else if (ep0_tail) {
                    Ep0Buffer[0] = FeatureBuf[ep0_off];
                    ep0_off++;
                    ep0_tail--;
                    ep0_next_in(1);
                } else {
                    UEP0_T_LEN = 0;
                    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                }
            } else {                                   /* OUT */
                if (ep0_set_report) {
                    ep0_set_report = 0;
                    app_feature(Ep0Buffer + ep0_set_off, FeatureBuf + 1);   /* 处理配置命令 */
                    UEP0_T_LEN = 0;
                    ep0_arm_in(0);                        /* 状态阶段 */
                }
                UEP0_CTRL = (UEP0_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
            }
        } else if (ep == 1) {
            if (token == UIS_TOKEN_IN) {
                UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                ep1_busy = 0;
            }
        }
    }

    if (fg & UIF_SUSPEND) {
        /* 只清标志；本设计不需要挂起唤醒逻辑 */
    }

    /* ------------------------------------------------------------------
     * 关键：CH552 的 USB_INT_FG 是「写 1 清零」。
     * 手册第 42 页：「直接位写 0 清零或寄存器对应位写 1 清零」
     *   —— 对整字节写 0 是**无效**的，标志会一直保持置位。
     *
     * 之前这里写的是 USB_INT_FG = 0x00，等于什么也没清。后果是
     * UIF_BUS_RST 永远为 1，于是每次 usb_poll() 都会重跑 ep_init()，
     * 把 UEP0_CTRL 反复复位成 R=ACK/T=NAK，主机发出的 SETUP 永远
     * 得不到数据响应。现象就是：设备能挂上总线（D+ 上拉已建立），
     * 但报「设备描述符请求失败」(VID_0000&PID_0002)。
     *
     * 正确做法：把本次已经处理过的标志原样写回（写 1 才清）。
     * ------------------------------------------------------------------ */
    USB_INT_FG = fg & (UIF_BUS_RST | UIF_TRANSFER | UIF_SUSPEND | UIF_FIFO_OV);
}

/* =========================================================================
 * 对外接口
 * ========================================================================= */
void usb_init(void)
{
    IE_EX &= ~IE_EX_USB;
    EA_DISABLE();

    USB_CTRL = bUC_RESET_SIE | bUC_CLR_ALL;
    USB_CTRL = 0x00;
    USB_INT_FG = 0x00;

    UDEV_CTRL = bUD_PD_DIS;        /* 全速；禁用内部 15k 下拉（默认值，别打开） */
    USB_CTRL &= ~bUC_LOW_SPEED;    /* 12Mbps */
    USB_DEV_AD = 0x00;

    ep_init();

    /* Feature 报告总线上固定以报告 ID 开头 */
    FeatureBuf[0] = 0x10;

    /* 使能设备 + 内部 1.5k 上拉（SYS_CTRL1 = 1）
       bUC_DMA_EN   必须置 1：端点缓冲区在 xRAM，SIE 靠 DMA 读写。
       bUC_INT_BUSY 与官方例程 / wagiminator 实现一致地打开：
                    标志未清零前 SIE 自动 NAK 新事务，防止我们还在
                    处理 Ep0Buffer 时被下一个包覆盖。此前把它关掉是
                    错误判断，标志清零已按写 1 语义修正，不会再卡死。 */
    USB_CTRL |= bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;
    UDEV_CTRL |= bUD_PORT_EN;

    USB_INT_EN = bUIE_BUS_RST | bUIE_TRANSFER | bUIE_SUSPEND;  /* 只用来置标志 */
    IE_EX &= ~IE_EX_USB;          /* 不开 USB 中断：没有向量，靠 usb_poll() 轮询 */
    EA_ENABLE();
}

uint8_t usb_configured(void) { return usb_cfg != 0; }

/* 发一包 EP1 IN：buf[0] 是 Report ID
 * 关中断保护：ep1_busy 的检查与缓冲区写入必须原子，否则可能在
 * "上一包刚发完" 的瞬间覆盖了正在发送的缓冲区。 */
uint8_t usb_ep1_send(const uint8_t *buf, uint8_t len)
{
    uint8_t i;
    uint8_t ea;

    if (len > 64) len = 64;

    ea = EA;
    EA = 0;
    if (ep1_busy || !usb_cfg) { EA = ea; return 0; }
    for (i = 0; i < len; i++) Ep1Buffer[i] = buf[i];
    ep1_busy = 1;
    UEP1_T_LEN = len;
    UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
    EA = ea;
    return 1;
}
