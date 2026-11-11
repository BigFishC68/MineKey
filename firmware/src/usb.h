/* =========================================================================
 * usb.h
 * ========================================================================= */
#ifndef USB_H
#define USB_H

#include <stdint.h>

/* 报告 ID */
#define REP_KBD     0x01
#define REP_MEDIA   0x02
#define REP_STATUS  0x03
#define REP_MOUSE   0x04
#define REP_HOST    0x05
#define REP_CONFIG  0x10

void    usb_init(void);
void    usb_poll(void);
uint8_t usb_configured(void);
uint8_t usb_ep1_send(const uint8_t *buf, uint8_t len);

extern __xdata uint8_t FeatureBuf[64];  /* [0]=报告 ID 0x10，[1..63]=负载 */

#endif
