#ifndef __LASER_RS485_H__
#define __LASER_RS485_H__

#include "laser-common.h"
int laser_stopclear(void);
int laser_on(void);
int laser_con_measure(uint32_t val);

#endif
