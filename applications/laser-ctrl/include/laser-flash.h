#ifndef __LASER__FLASH_H__
#define __LASER__FLASH_H__

#include "laser-common.h"

int laser_flash_write(uint16_t address, uint32_t val);
int laser_flash_read(uint16_t address, uint32_t *val);
int laser_flash_read_mode(void);
int laser_flash_write_mode(void);

#endif
