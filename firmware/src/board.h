/* =========================================================================
 * board.h —— 板级定义（对应 keyboard.eprj2 原理图）
 *
 * 引脚核对表（CH552G SOP-16）：
 *   1  P3.2  SA      编码器 A
 *   2  P1.4  ROW2    矩阵行 2
 *   3  P1.5  ROW3    矩阵行 3
 *   4  P1.6  COL3    矩阵列 3
 *   5  P1.7  COL4    矩阵列 4
 *   6  RST   NC      内部下拉
 *   7  P3.1  COL1    矩阵列 1
 *   8  P3.0  ROW1    矩阵行 1
 *   9  P1.1  COL2    矩阵列 2
 *  10  P3.3  LED_DIN WS2812 数据
 *  11  P3.4  SB      编码器 B
 *  12  P3.6  UDP     USB D+
 *  13  P3.7  UDM     USB D-
 *  14  GND
 *  15  VCC
 *  16  V33   3.3V 内部 LDO 输出（仅 100nF）
 * ========================================================================= */
#ifndef BOARD_H
#define BOARD_H

#include "ch552.h"

/* ---------------- 矩阵尺寸 ---------------- */
#define MAT_ROWS      3
#define MAT_COLS      4
#define MAT_BITS      (MAT_ROWS * MAT_COLS)   /* 12 个位置 */
#define NUM_KEYS      11                      /* 11 个轴，第 12 位是编码器按压 */
#define ENC_BIT       3                       /* bit3 = (R1,C4) = 编码器按压 */
#define NUM_LEDS      11

/* 鼠标移动（功能类型 0x07）：每次发 MOVE_STEP 像素，按住每 MOVE_PERIOD_MS 发一次
   低字节编码 = (档位 << 4) | 方向，像素 = MOVE_STEP << 档位
   即 6 / 12 / 24 px 每 20ms（300 / 600 / 1200 px/s），老配置 0x0701 落在档位 0。
   在这之上还有"连续点击提速"：快速双击 ×2、三击 ×3（见 mouse_move_tick） */
#define MOVE_STEP        6
#define MOVE_PERIOD_MS   20
#define MOVE_LEVEL_MAX   2
#define MOVE_TAP_WINDOW  20            /* 松手后 20×20ms=400ms 内再按算连击 */
#define MOVE_STEP_MAX    60            /* 单包位移上限，不能越过有符号字节 */

/* 灯珠总电流上限（sum = R+G+B 之和）：400mA ≈ 5200
   估算模型：11mA + 19mA × sum/255；配置器侧用同一阈值，超了不允许下发 */
#define LED_SUM_LIMIT 5200

/* ---------------- 引脚 ---------------- */
#define ROW1_PIN      P3_0
#define ROW2_PIN      P1_4
#define ROW3_PIN      P1_5

#define COL1_PIN      P3_1
#define COL2_PIN      P1_1
#define COL3_PIN      P1_6
#define COL4_PIN      P1_7

#define ENC_A_PIN     P3_2
#define ENC_B_PIN     P3_4

#define LED_PIN       P3_3
__sbit __at (0xB3) LED_BIT;              /* LED_PIN 的位地址，供汇编/快速操作 */

/* ---------------- 设备身份 ---------------- */
#define USB_VID       0x1209              /* pid.codes 测试 VID */
#define USB_PID       0x0001
#define FW_VERSION_H  0
#define FW_VERSION_L  10
#define PROTO_VERSION 1

/* ---------------- 动作码（与配置器一致） ----------------
 * 5/6（重新加载配置 / 恢复出厂）已废弃：这类操作只允许配置器通过 Feature
 * 通道发起，不再接受从按键触发。 */
#define ACT_FN_LAYER  1
#define ACT_BRI_UP    2
#define ACT_BRI_DOWN  3
#define ACT_LED_NEXT  4

/* ---------------- 配置结构（存 DataFlash） ---------------- */
#define CFG_MAGIC     0x4B46              /* 'K','F' */
#define CFG_VERSION   2                   /* 布局/键序变更时 +1：旧配置会被判无效并回出厂值 */
#define CFG_OFFSET    0
#define CFG_SIZE      96                  /* <= 128 字节 DataFlash */

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  enc_invert;
    uint16_t keys[NUM_KEYS];              /* 每个轴的功能码 */
    uint16_t enc[3];                      /* 0=CW 1=CCW 2=按压 */
    uint8_t  led_mode;
    uint8_t  led_bright;
    uint8_t  led_speed;
    uint8_t  led_rgb[3];
    uint8_t  per_key[NUM_LEDS][3];
    uint8_t  checksum;                    /* 前面所有字节的 XOR */
} keyboard_config_t;

#endif /* BOARD_H */
