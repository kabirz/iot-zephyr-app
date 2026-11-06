#include <laser-can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/app_version.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
LOG_MODULE_REGISTER(firmware_up, LOG_LEVEL_INF);

#if DT_NODE_EXISTS(DT_NODELABEL(spi_laser_fpga))
extern uint32_t fpga_uint32_version(void);
#else
static inline uint32_t fpga_uint32_version(void)
{
	LOG_WRN("fpga not present on this board");
	return 0;
}
#endif

static void fw_can_recevie(uint32_t code, uint32_t offset)
{
	struct can_frame frame = {
		.id = PLATFORM_TX,
		.data_32 = {code, offset},
		.dlc = can_bytes_to_dlc(8),
	};

	laser_can_send(&frame);
}

int fw_update(struct can_frame *frame)
{
	static struct image_fw_msg msg;
	uint32_t size = can_dlc_to_bytes(frame->dlc);

	if (frame->id == PLATFORM_RX) {
		if (frame->data_32[0] == BOARD_START_UPDATE) {
			atomic_set_bit(&laser_status, LASER_FW_UPDATE);
			if (size != 8) {
				LOG_ERR("start update message size must 8");
				fw_can_recevie(FW_CODE_FLASH_ERROR, 0);
				atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
				return -1;

			}
			memset(&msg, 0, sizeof(msg));
			if (flash_img_init(&msg.flash_img_ctx) != 0) {
				LOG_ERR("flash init failed");
				fw_can_recevie(FW_CODE_FLASH_ERROR, 0);
				atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
				return -1;
			}
			if (flash_area_flatten(msg.flash_img_ctx.flash_area, 0, msg.flash_img_ctx.flash_area->fa_size) != 0) {
				LOG_ERR("flash erase failed");
				fw_can_recevie(FW_CODE_FLASH_ERROR, 0);
				atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
				return -1;
			}
			msg.offset = 0;
			msg.total_size = frame->data_32[1];
			fw_can_recevie(FW_CODE_OFFSET, 0);
			LOG_INF("Start upgrade firmware, size:%d", msg.total_size);
		} else if (frame->data_32[0] == BOARD_CONFIRM) {
			flash_img_buffered_write(&msg.flash_img_ctx, frame->data, 0, true);
			if (msg.total_size != flash_img_bytes_written(&msg.flash_img_ctx)) {
				LOG_ERR("Download failed total: %d, %d", msg.total_size, flash_img_bytes_written(&msg.flash_img_ctx));
				fw_can_recevie(FW_CODE_TRANFER_ERROR, msg.offset);
				return -1;
			} else {
				LOG_INF("Download Finished, need reboot and finish system upgrade.");
			}
			boot_request_upgrade(frame->data_32[1]);
			fw_can_recevie(FW_CODE_CONFIRM, 0x55AA55AA);
		} else if (frame->data_32[0] == BOARD_VERSION) {
			fw_can_recevie(FW_CODE_VERSION, APPVERSION);
		} else if (frame->data_32[0] == FPGA_VERSION) {
			fw_can_recevie(FW_CODE_FPGA_VERSION, fpga_uint32_version());
		} else if (frame->data_32[0] == BOARD_REBOOT) {
			sys_reboot(SYS_REBOOT_WARM);
		}
	} else if (frame->id == FW_DATA_RX) {
		if (flash_img_buffered_write(&msg.flash_img_ctx, frame->data, size, false)) {
			fw_can_recevie(FW_CODE_FLASH_ERROR, 0);
			atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
			return -1;
		}
		msg.offset += size;
		if (msg.offset == msg.total_size) {
			LOG_INF("recived firmware: %d/%d", msg.offset, msg.total_size);
			fw_can_recevie(FW_CODE_UPDATE_SUCCESS, msg.total_size);
			atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
		} else {
			if (msg.offset % 4096 == 0)
				LOG_INF("recived firmware: %d/%d", msg.offset, msg.total_size);
			if (msg.offset % 64 == 0)
				fw_can_recevie(FW_CODE_OFFSET, msg.offset);
		}
	}
	return 0;
}

