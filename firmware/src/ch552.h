/* =========================================================================
 * ch552.h —— CH552G 寄存器定义（SDCC / MCS51）
 *
 * 地址全部取自 CH552 数据手册：
 *   表 5.2  SFR 描述和复位值
 *   表 9.1.2 中断相关寄存器
 *   表 10.2.1 GPIO 寄存器
 *   表 16.2.1 / 16.3.1 USB 寄存器
 * ========================================================================= */
#ifndef CH552_H
#define CH552_H

#include <stdint.h>

#ifdef HOST_CHECK
#include "host_stub.h"
#else
#include <8051.h>
#endif

/* ---------------- 系统 / 时钟 ---------------- */
__sfr __at (0xA1) SAFE_MOD;      /* 安全模式：先写 0x55 再写 0xAA */
__sfr __at (0xB1) GLOBAL_CFG;    /* bit2 = bDATA_WE, bit3 = bCODE_WE */
__sfr __at (0xB9) CLOCK_CFG;
__sfr __at (0xFF) WDOG_COUNT;
__sfr __at (0xA9) WAKE_CTRL;
__sfr __at (0x86) ROM_CTRL;      /* 写 */
__sfr __at (0x86) ROM_STATUS;    /* 读 */
__sfr __at (0x84) ROM_ADDR_L;
__sfr __at (0x85) ROM_ADDR_H;
__sfr __at (0x8E) ROM_DATA_L;
__sfr __at (0x8F) ROM_DATA_H;

/* ---------------- GPIO ---------------- */
__sfr __at (0x92) P1_MOD_OC;     /* 0 = 推挽, 1 = 开漏 */
__sfr __at (0x93) P1_DIR_PU;     /* 推挽下: 0=输入 1=输出；开漏下: 0=无上拉 1=有上拉 */
__sfr __at (0x96) P3_MOD_OC;
__sfr __at (0x97) P3_DIR_PU;
__sfr __at (0xC6) PIN_FUNC;
__sfr __at (0xA2) XBUS_AUX;

__sbit __at (0x90) P1_0;
__sbit __at (0x91) P1_1;
__sbit __at (0x92) P1_2;
__sbit __at (0x93) P1_3;
__sbit __at (0x94) P1_4;
__sbit __at (0x95) P1_5;
__sbit __at (0x96) P1_6;
__sbit __at (0x97) P1_7;

__sbit __at (0xB0) P3_0;
__sbit __at (0xB1) P3_1;
__sbit __at (0xB2) P3_2;
__sbit __at (0xB3) P3_3;
__sbit __at (0xB4) P3_4;
__sbit __at (0xB5) P3_5;
__sbit __at (0xB6) P3_6;
__sbit __at (0xB7) P3_7;

/* ---------------- 中断 ---------------- */
__sfr __at (0xE8) IE_EX;
__sfr __at (0xE9) IP_EX;
__sfr __at (0xC7) GPIO_IE;

#define IE_EX_USB        0x04
#define INT_NO_TMR0      1
#define INT_NO_USB       8

/* ---------------- USB ---------------- */
__sfr __at (0xD1) UDEV_CTRL;
__sfr __at (0xD8) USB_INT_FG;
__sfr __at (0xD9) USB_INT_ST;
__sfr __at (0xDA) USB_MIS_ST;
__sfr __at (0xDB) USB_RX_LEN;
__sfr __at (0xE1) USB_INT_EN;
__sfr __at (0xE2) USB_CTRL;
__sfr __at (0xE3) USB_DEV_AD;

__sfr __at (0xDC) UEP0_CTRL;
__sfr __at (0xDD) UEP0_T_LEN;
__sfr __at (0xD2) UEP1_CTRL;
__sfr __at (0xD3) UEP1_T_LEN;
__sfr __at (0xEA) UEP4_1_MOD;
__sfr __at (0xEB) UEP2_3_MOD;

/* 端点 DMA 地址寄存器：按字节操作，避免 sfr16 高低位顺序的平台差异 */
__sfr __at (0xEC) UEP0_DMA_L;
__sfr __at (0xED) UEP0_DMA_H;
__sfr __at (0xEE) UEP1_DMA_L;
__sfr __at (0xEF) UEP1_DMA_H;

#define SET_UEP0_DMA(a)  do { UEP0_DMA_L = (uint8_t)((uint16_t)(a) & 0xFF); \
                               UEP0_DMA_H = (uint8_t)(((uint16_t)(a) >> 8) & 0x03); } while (0)
#define SET_UEP1_DMA(a)  do { UEP1_DMA_L = (uint8_t)((uint16_t)(a) & 0xFF); \
                               UEP1_DMA_H = (uint8_t)(((uint16_t)(a) >> 8) & 0x03); } while (0)

/* ---- UDEV_CTRL ---- */
#define bUD_PD_DIS       0x80
#define bUD_LOW_SPEED    0x04
#define bUD_PORT_EN      0x01

/* ---- USB_CTRL ---- */
#define bUC_LOW_SPEED    0x40
#define bUC_DEV_PU_EN    0x20   /* 同时是 bUC_SYS_CTRL1 */
#define bUC_SYS_CTRL0    0x10
#define bUC_INT_BUSY     0x08
#define bUC_RESET_SIE    0x04
#define bUC_CLR_ALL      0x02
#define bUC_DMA_EN       0x01

/* ---- USB_INT_FG ---- */
#define UIF_BUS_RST      0x01
#define UIF_TRANSFER     0x02
#define UIF_SUSPEND      0x04
#define UIF_FIFO_OV      0x10

/* ---- USB_INT_EN ---- */
#define bUIE_BUS_RST     0x01
#define bUIE_TRANSFER    0x02
#define bUIE_SUSPEND     0x04

/* ---- USB_INT_ST ---- */
#define MASK_UIS_ENDP    0x0F
#define MASK_UIS_TOKEN   0x30
#define UIS_TOKEN_OUT    0x00
#define UIS_TOKEN_SOF    0x10
#define UIS_TOKEN_IN     0x20
#define UIS_TOKEN_SETUP  0x30

/* ---- UEPn_CTRL ---- */
#define bUEP_R_TOG       0x80
#define bUEP_T_TOG       0x40
#define bUEP_AUTO_TOG    0x10
#define MASK_UEP_R_RES   0x0C
#define MASK_UEP_T_RES   0x03

#define UEP_R_RES_ACK    0x00
#define UEP_R_RES_TOUT   0x04
#define UEP_R_RES_NAK    0x08
#define UEP_R_RES_STALL  0x0C

#define UEP_T_RES_ACK    0x00
#define UEP_T_RES_TOUT   0x01
#define UEP_T_RES_NAK    0x02
#define UEP_T_RES_STALL  0x03

/* ---------------- 常用小工具 ---------------- */
#define EA_DISABLE()  do { EA = 0; } while (0)
#define EA_ENABLE()   do { EA = 1; } while (0)
#ifdef HOST_CHECK
#define NOP()         do { } while (0)
#else
#define NOP()         do { __asm nop __endasm; } while (0)
#endif

static inline void safe_mode_enter(void) { SAFE_MOD = 0x55; SAFE_MOD = 0xAA; }

#endif /* CH552_H */
