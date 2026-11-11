/* =========================================================================
 * host_stub.h —— 仅用于在 PC 上做语法/类型检查（gcc -DHOST_CHECK）
 *
 * 把 SDCC 的内存空间关键字和 SFR 伪造成普通变量，这样 app.c / usb.c
 * 可以在 x86 上编译一遍，把拼写错误、类型不匹配、未声明变量抓出来。
 * 它不模拟任何硬件行为，也不能替代真机调试。
 * ========================================================================= */
#ifndef HOST_STUB_H
#define HOST_STUB_H

#include <stdint.h>

#define __at(x)
#define __sfr       static volatile uint8_t
#define __sfr16     static volatile uint16_t
#define __sbit      static volatile uint8_t
#define __code
#define __xdata
#define __data
#define __interrupt(n)
#define __using(n)
#define __naked
#define __reentrant

/* 标准 8051 里由 <8051.h> 提供的寄存器 */
static volatile uint8_t P1, P3;
static volatile uint8_t EA, ET0, TR0, TMOD, TH0, TL0, IE, SP, ACC, B, PSW;

/* 让 app.c 里的 main() 不与测试程序的 main 冲突 */
#define main firmware_main

#endif
