
#ifndef __MOD_GPIO_H__
#define __MOD_GPIO_H__
#include <stdbool.h>
#include <common.h>
battery_status_t read_battery_status(void);
int gpio_init(void);
void connect_switch(uint8_t type);
void can_power_enable(bool up);
void dis_power_enable(bool up);
void handler_power_enable(bool up);
int handler_get_btn(void);
void system_sleep(void);
#endif
