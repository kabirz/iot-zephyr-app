#ifndef __LASER_COMMON_H__
#define __LASER_COMMON_H__
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

enum fw_error_code {
	FW_CODE_OFFSET,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIRM,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
	FW_CODE_FPGA_VERSION,
};

enum board_option {
	BOARD_START_UPDATE,
	BOARD_CONFIRM,
	BOARD_VERSION,
	BOARD_REBOOT,
	FPGA_VERSION,
};

enum {
	LASER_DEVICE_STATUS,
	LASER_WRITE_MODE,
	LASER_NEED_CLOSE,
	LASER_ON,
	LASER_CON_MESURE,
	LASER_FW_UPDATE,
};
extern atomic_t laser_status;
extern uint64_t latest_fw_up_times;

struct laser_encode_data {
	int64_t encode1: 20;
	int64_t encode2: 20;
	uint64_t laser_val: 24;
};

int laser_get_encode_data(struct laser_encode_data *val);


#endif
