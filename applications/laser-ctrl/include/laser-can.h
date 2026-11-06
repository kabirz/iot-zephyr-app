#ifndef __LASER_CAN_H__
#define __LASER_CAN_H__
#include "laser-common.h"

enum {
	COB_ID1_RX = 0x664,
	COB_ID1_TX = 0x5E4,
	COB_ID2_RX = 0x665,
	COB_ID2_TX = 0x5E5,

	PLATFORM_RX   = 0x101,
	PLATFORM_TX   = 0x102,
	FW_DATA_RX    = 0x103,
};

#define CANRECSDOREADXAXISREALVAL  0x40466200
#define CANRECSDOREADYAXISREALVAL  0x40486200
#define CANRECSDOREADYAXIS         0x40026200
#define CANRECSDOWRITEYAXIS        0x22026200
#define CANRECSDOREADXAXIS         0x40016200
#define CANRECSDOWRITEXAXIS        0x22016200
#define CANRECSDOREADCONWORD       0x40406200
#define CANRECSDOWRITECONWORD      0x22406200
#define CANRECSDOWRITECONWORD_NEW  0x23406200
#define CANCMD_LASER_CTRL          0x22416200
#define CANCMD_LASER_CTRL_NEW      0x23416200
#define CANCMD_LASER_PEROID_CONF   0x22426200
#define CANCMD_LASER_PEROID_CONFN  0x23426200
#define CANCMD_LASER_PEROID_GET    0x40426200
#define CANWRITESYSTEMSTATUS       0x22000000
#define CANWRITESYSTEMSTATUS_NEW   0x23000000
#define CANREADSYSTEMSTATUS        0x40000000

#define CANCMD_LASER_CTRL_ENABLE   0x00000001
#define CANCMD_LASER_CTRL_DISABLE  0x00000000

#define CAN_HOST_MASK              0x00FFFFFF
#define CAN_HOST_ACK_ID            0x60000000
#define CAN_HOST_NOACK_ID          0x80000000

#define SYSTEMSTATUSINIT           0x00    // init status
#define SYSTEMSTATUSWORKING        0x01    // working status
#define SYSTEMSTATUSWORKREADERR    0x02    // init eeprom error
#define SYSTEMSTATUSINITERR        0x03    // init parameters error
#define SYSTEMSTATUSEEPROM         0x10    // eeprom writing status
#define SYSTEMSTATUSWRIERR         0x12    // writing eeprom error
#define SYSTEMSTATUSREADERR        0x11    // reading eeprom error

int cob_msg_send(uint32_t data1, uint32_t data2, uint32_t id);
int laser_can_send(struct can_frame *frame);

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>

struct image_fw_msg {
	uint32_t total_size;
	uint32_t offset;
	struct flash_img_context flash_img_ctx;
};
int fw_update(struct can_frame *frame);

#endif
