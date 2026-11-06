#include <zephyr/kernel.h>
#include <laser-can.h>
#include <laser-rs485.h>
#include <laser-flash.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(laser_can_msgq, 8);
#define USER_NODE DT_PATH(zephyr_user)
#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
static struct k_work_delayable laser_delayed_work;
#endif
uint64_t latest_fw_up_times;
static uint32_t SystemStatus = SYSTEMSTATUSWORKING;
int16_t gcXaxisInitValue, gcYaxisInitValue;
atomic_t laser_status = ATOMIC_INIT(0);

int cob_msg_send(uint32_t data1, uint32_t data2, uint32_t id)
{
	struct can_frame frame = {
		.data_32 = {
			sys_cpu_to_be32(data1),
			sys_cpu_to_be32(data2),
		},
		.id = id,
		.dlc = can_bytes_to_dlc(8),
	};


	return laser_can_send(&frame);
}

static void laser_canrx_msg_handler(struct can_frame *frame)
{
	uint32_t ack_cmd;
	static uint32_t LaserPeriod = 100;

	switch (frame->id) {
	case COB_ID1_RX : {
		switch (sys_be32_to_cpu(frame->data_32[0])) {
		case CANRECSDOREADCONWORD:
			cob_msg_send(0x43406200, atomic_test_bit(&laser_status, LASER_DEVICE_STATUS), COB_ID1_TX);
			break;
		case CANRECSDOWRITECONWORD:
		case CANRECSDOWRITECONWORD_NEW:
			if (sys_be32_to_cpu(frame->data_32[1])) {
				LOG_DBG("device running");
				atomic_set_bit(&laser_status, LASER_DEVICE_STATUS);
			} else {
				LOG_DBG("device not running");
				atomic_clear_bit(&laser_status, LASER_DEVICE_STATUS);
			}
			cob_msg_send(0x60406200, sys_be32_to_cpu(frame->data_32[1]), COB_ID1_TX);
			break;
		case CANRECSDOREADYAXISREALVAL:
		case CANRECSDOREADXAXISREALVAL: {
			if (sys_be32_to_cpu(frame->data_32[0]) == CANRECSDOREADXAXISREALVAL)
				cob_msg_send(0x43466200, 0x1234, COB_ID1_TX);
			else
				cob_msg_send(0x43486200, 0x1234, COB_ID1_TX);
			}
			break;
		case CANRECSDOREADXAXIS:
			cob_msg_send(0x43016200, gcXaxisInitValue, COB_ID1_TX);
			break;
		case CANRECSDOREADYAXIS:
			cob_msg_send(0x43026200, gcYaxisInitValue, COB_ID1_TX);
			break;
		case CANRECSDOWRITEXAXIS:
			gcXaxisInitValue = sys_be32_to_cpu(frame->data_32[1]) & 0xFFFF;
			cob_msg_send(0x60016200, gcXaxisInitValue, COB_ID1_TX);
			break;
		case CANRECSDOWRITEYAXIS:
			gcYaxisInitValue = sys_be32_to_cpu(frame->data_32[1]) & 0xFFFF;
			cob_msg_send(0x60026200, gcYaxisInitValue, COB_ID1_TX);
			break;
		case CANREADSYSTEMSTATUS:
			SystemStatus = SYSTEMSTATUSWORKING;
			LOG_DBG("mem read mode");
			laser_flash_read_mode();
			cob_msg_send(SystemStatus, CANREADSYSTEMSTATUS, COB_ID1_TX);
			break;
		case CANWRITESYSTEMSTATUS:
		case CANWRITESYSTEMSTATUS_NEW:
			SystemStatus = SYSTEMSTATUSEEPROM;
			LOG_DBG("mem write mode");
			laser_flash_write_mode();
			cob_msg_send(SystemStatus, CANWRITESYSTEMSTATUS, COB_ID1_TX);
			break;
		case CANCMD_LASER_CTRL:
		case CANCMD_LASER_CTRL_NEW:
			ack_cmd = (CANCMD_LASER_CTRL & CAN_HOST_MASK)|CAN_HOST_NOACK_ID;
			if (sys_be32_to_cpu(frame->data_32[1]) == CANCMD_LASER_CTRL_ENABLE) {
				LOG_DBG("laser on and measure");
				if (laser_on() == 0) {
					if (laser_con_measure(LaserPeriod) == 0)
						ack_cmd = (CANCMD_LASER_CTRL & CAN_HOST_MASK)|CAN_HOST_ACK_ID;
				}
			} else if (sys_be32_to_cpu(frame->data_32[1]) == CANCMD_LASER_CTRL_DISABLE) {
#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
				if (atomic_test_bit(&laser_status, LASER_ON) &&
					atomic_test_bit(&laser_status, LASER_CON_MESURE)) {
					atomic_set_bit(&laser_status, LASER_NEED_CLOSE);
					k_work_schedule(&laser_delayed_work, K_SECONDS(2));
				} else
#endif
				{
					if (laser_stopclear() == 0)
						ack_cmd = (CANCMD_LASER_CTRL & CAN_HOST_MASK)|CAN_HOST_ACK_ID;
				}
				LOG_DBG("laser stop");
			}
			cob_msg_send(ack_cmd, sys_be32_to_cpu(frame->data_32[1]), COB_ID1_TX);
			break;
		case CANCMD_LASER_PEROID_CONF:
		case CANCMD_LASER_PEROID_CONFN:
			ack_cmd = (CANCMD_LASER_PEROID_CONF & CAN_HOST_MASK)|CAN_HOST_ACK_ID;
			LaserPeriod = sys_be32_to_cpu(frame->data_32[1]);
			LOG_DBG("set period: %d", LaserPeriod);
			cob_msg_send(ack_cmd, sys_be32_to_cpu(frame->data_32[1]), COB_ID1_TX);
			break;
		case CANCMD_LASER_PEROID_GET:
			LOG_DBG("get period: %d", LaserPeriod);
			ack_cmd = (CANCMD_LASER_PEROID_GET & CAN_HOST_MASK)|CAN_HOST_ACK_ID;
			cob_msg_send(ack_cmd, LaserPeriod, COB_ID1_TX);
			break;
		default:
			LOG_ERR("Unknown command: 0x%x", sys_be32_to_cpu(frame->data_32[0]));
			break;
		}
	} break;
	case COB_ID2_RX : {
		if (frame->data[0] == 0x22) {
			uint32_t address = frame->data[1] + frame->data[2] * 10;
			uint32_t val = sys_be32_to_cpu(frame->data_32[1]);
			if (address == 0) address = 1;
			int ret = laser_flash_write(address-1, val);

			LOG_DBG("address: %d, val: 0x%x", address, val);
			if (ret) {
				cob_msg_send(0x12, val, COB_ID2_TX);
			} else {
				cob_msg_send(1, val, COB_ID2_TX);
			}
		} else if (frame->data[0] == 0x40) {
			uint32_t address = frame->data[1] + frame->data[2] * 10;
			uint32_t val;

			if (address == 0) address = 1;

			int ret = laser_flash_read(address - 1, &val);

			LOG_DBG("address: %d, val: 0x%x", address, val);
			if (ret) {
				cob_msg_send(0x11, val, COB_ID2_TX);
			} else {
				cob_msg_send(1, val, COB_ID2_TX);
			}
		}
	}
		break;
	case PLATFORM_RX:
	case FW_DATA_RX:
		latest_fw_up_times = k_uptime_get();
		fw_update(frame);
		break;
	default:
		LOG_ERR("can frame id (0x%x) is not supported", frame->id);
	}
}

static void laser_cantx_callback(const struct device *dev, int error, void *user_data)
{
	uint32_t count = (uint32_t)(uintptr_t)user_data;
	if (error == 0) {
		if (!atomic_test_bit(&laser_status, LASER_FW_UPDATE)) {
			LOG_DBG("CAN frame #%u successfully sent", count);
		}
	} else {
		LOG_ERR("failed to send CAN frame #%u (err %d)", count, error);
	}
}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
static void laser_stop_work_handler(struct k_work *work)
{
	if (atomic_test_and_clear_bit(&laser_status, LASER_NEED_CLOSE)) {
		laser_stopclear();
	}
}
#endif

static atomic_t laser_can_tx_seq;

int laser_can_send(struct can_frame *frame)
{
	uint32_t seq = atomic_inc(&laser_can_tx_seq) + 1;

	return can_send(can_dev, frame, K_MSEC(100), laser_cantx_callback, (void *)(uintptr_t)seq);
}

bool check_can_device_ready(void)
{
	return DEVICE_API_IS(can, can_dev) && device_is_ready(can_dev);
}

int laser_can_init(void)
{
	int err = -1;
	uint8_t can_check_time = 0;

	while (can_check_time++ < 10) {
		if (check_can_device_ready()) break;
	}
	if (can_check_time >= 10) {
		LOG_ERR("can device is not ready");
		goto end;
	}

	if ((err = can_set_bitrate(can_dev, 250000)) != 0) {
		LOG_ERR("failed to set bitrate (err %d)", err);
		goto end;
	}

	if ((err = can_start(can_dev)) != 0) {
		LOG_ERR("failed to start CAN controller (err %d)", err);
		goto end;
	}

	laser_flash_read_mode();

	struct can_filter filter = {
		.mask = CAN_STD_ID_MASK
	};

	filter.id = COB_ID1_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);

	filter.id = COB_ID2_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);

	filter.id = PLATFORM_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);

	filter.id = FW_DATA_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);
end:
	return err;
}

void laser_can_process_thread(void)
{
	struct can_frame frame;

	if (laser_can_init() != 0) {
		LOG_ERR("can init failed");
		return;
	}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	k_work_init_delayable(&laser_delayed_work, laser_stop_work_handler);
	k_work_schedule(&laser_delayed_work, K_SECONDS(2));
#endif

	while (true) {
		if (k_msgq_get(&laser_can_msgq, &frame, K_FOREVER) == 0)
			laser_canrx_msg_handler(&frame);
	}
}

K_THREAD_DEFINE(laser_can, 2048, laser_can_process_thread, NULL, NULL, NULL, 11, 0, 0);
