/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_BOARD_IO_H
#define APOLLO_BOARD_IO_H

#include <stdbool.h>
#include <stdint.h>

/* 板载基础 IO 初始化 (LED/按键/蜂鸣器静音/ADC) */
int board_io_init(void);

/* LED: 0 = DS0 红 (PB1), 1 = DS1 绿 (PB0), 均低电平点亮 */
int board_io_led_set(unsigned idx, bool on);
int board_io_led_toggle(unsigned idx);

/* 蜂鸣器 (PCF8574 P0, 低 = 响), 阻塞响 ms 毫秒 */
int board_io_beep(uint16_t ms);

/* 蜂鸣器连续开关 (报警/界面控制用, 不阻塞) */
int board_io_beeper_set(bool on);

/* 按键实时电平 (true = 按下) */
bool board_io_key_is_down(unsigned idx);

/* AP3216C: 光照 (ALS, unit-less data) 与接近 (PS 0-1023)。
 * 返回 0 成功; 传感器不在位返回 -ENODEV */
int board_io_als_read(uint16_t *als);
int board_io_ps_read(uint16_t *ps);

/* ADC1 PA5 (INP19) 单次采样, 返回毫伏值; 负值为错误 */
int board_io_adc_mv(void);

/* 按键累计事件数 (自上电起), 返回 false 表示该键不存在 */
bool board_io_key_count(unsigned idx, uint32_t *count);

#endif /* APOLLO_BOARD_IO_H */
