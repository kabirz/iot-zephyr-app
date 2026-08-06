#ifndef _MOD_DISPLAY_H
#define _MOD_DISPLAY_H
#include <common.h>

void mod_display_reinit(void);
int mod_display_init(void);
void mod_display_clear(void);
/* 2.4G (nRF24L01+) 无线模式信号显示 */
void mod_display_rf24(uint8_t rssi);
void mod_display_battery(uint32_t mv, battery_status_t status);
void mod_display_can(void);

void mod_display_scanner(const scanner_data_t *s);
void mod_display_all(const global_params_t *params);

#endif
