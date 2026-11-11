/* =========================================================================
 * app.h
 * ========================================================================= */
#ifndef APP_H
#define APP_H

#include <stdint.h>

void app_init(void);
void app_tick(void);                                  /* 1ms 节拍里调用 */
void app_feature(const uint8_t *in, uint8_t *out);    /* HID Feature 命令入口 */

#endif
