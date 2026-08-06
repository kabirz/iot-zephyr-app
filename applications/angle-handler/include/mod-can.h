#ifndef __MOD_CAN_H__
#define __MOD_CAN_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <common.h>

enum {
	RF24_CONFIG_CMD = 0x110,    /* 平台→手柄: 查询 rf24 地址 (信道固定为 1) */
	RF24_CONFIG_RESP = 0x111,   /* 手柄→平台: rf24 配置响应 */
	COBID_HEATBEAT = 0x763,
	TEST_FRAME = 0x777,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
};

/* RF24_CONFIG_CMD 命令类型 */
enum rf24_config_cmd {
	RF24_CMD_GET_CONFIG = 0x02,  /* 查询配置: [cmd 1B][reserved 7B] */
};

int mod_can_send(struct can_frame *frame);

/**
 * @brief 解析扫描仪 CAN 数据 (0x263/0x363/0x463)
 *
 * @param frame CAN 接收帧
 */
void mod_can_parse_scanner(struct can_frame *frame);

#endif
