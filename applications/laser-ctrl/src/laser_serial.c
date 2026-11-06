#include "laser-can.h"
#include <zephyr/device.h>
#include <stdlib.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <laser-common.h>
LOG_MODULE_REGISTER(laser_serial, LOG_LEVEL_INF);

int laser_stopclear(void);
#define USER_NODE DT_PATH(zephyr_user)
static const struct device *uart_dev = DEVICE_DT_GET(DT_ALIAS(laser_serial));

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
static const struct gpio_dt_spec rs485tx_gpios = GPIO_DT_SPEC_GET(USER_NODE, rs485_tx_gpios);
#endif
struct rx_buf {
	uint32_t len;
	uint8_t data[128];
};
K_MSGQ_DEFINE(laser_serial_msgq, sizeof(struct rx_buf), 3, 4);
static K_EVENT_DEFINE(laser_event);
#define EVENT_LASER_STOP_ON BIT(0)
#define EVENT_LASER_ERROR   BIT(1)
#define EVENT_LASER_MSG     BIT(2)
#define EVENT_LASER_OTHER   BIT(3)
static uint8_t id;
static bool first_on;
static bool enable_log;
static uint8_t tx_buf[256];
struct error_msg {
	uint16_t code;
	char *code_desc;
} error_msg_list [] = {
	{  0, "No Error"},
	{200, "sensor boot"},
	{203, "error command"},
	{210, "sensors not in tracking mode"},
	{211, "track time too short"},
	{212, "fisrt run stop/clear then run this command"},
	{220, "serail data error"},
};

static char *get_error_desc(uint16_t code)
{
	for (size_t i = 0; i < ARRAY_SIZE(error_msg_list); i++) {
		if (error_msg_list[i].code == code) {
			return error_msg_list[i].code_desc;
		}
	}
	return "Unknown error";
}

static void laser_msg_process_thread(void)
{
	int32_t distance;
	struct rx_buf buf;

	while (true) {
		if (k_msgq_get(&laser_serial_msgq, &buf, K_FOREVER) == 0) {
			int i = -1;
			for (int j = 0; j + 1 < buf.len; j++) {
				if (buf.data[j] == 'g' && buf.data[j + 1] == id) {
					i = j;
					break;
				}
			}
			if (i < 0 || i + 2 >= buf.len) {
				LOG_ERR("Invalid uart frame!");
				continue;
			}
			if (buf.data[i + 2] == 'h') { // Distance
				buf.data[buf.len - 2] = '\0';
				distance = strtol(buf.data + 3 + i, NULL, 10);
				if (enable_log)
					LOG_INF("distance: %d", distance);
				if (first_on) {
					first_on = false;
					k_event_set(&laser_event, EVENT_LASER_MSG);
				}
			} else if (buf.data[i + 2] == '@' && i + 3 < buf.len &&
				   buf.data[i + 3] == 'E') { // Error code
				buf.data[buf.len - 2] = '\0';
				uint16_t err_code = strtol(buf.data + 4 + i, NULL, 10);
				LOG_ERR("laser error code: %s(%d)", get_error_desc(err_code),
					err_code);
				distance = 0;
				if (!atomic_test_bit(&laser_status, LASER_CON_MESURE))
					k_event_set(&laser_event, EVENT_LASER_ERROR);
			} else if (buf.data[i + 2] == '?') { // stop/clear
				if (!atomic_test_bit(&laser_status, LASER_CON_MESURE)) {
					first_on = true;
					LOG_INF("laser on reply");
				} else {
					first_on = false;
					LOG_INF("laser stop/clear reply");
				}
				k_event_set(&laser_event, EVENT_LASER_STOP_ON);
				continue;
			} else {
				k_event_set(&laser_event, EVENT_LASER_OTHER);
				continue;
			};
			if (!atomic_test_bit(&laser_status, LASER_FW_UPDATE)) {
				struct laser_encode_data laser_data;
				struct can_frame frame = {
					.id = 0x2E4,
					.dlc = can_bytes_to_dlc(8),
				};
#if defined(CONFIG_BOARD_LASER_F103RET7)
				laser_get_encode_data(&laser_data);
#else
				laser_data.encode1 = 0x12345;
				laser_data.encode2 = 0x67899;
#endif
				laser_data.laser_val = distance;
				memcpy(frame.data, &laser_data, 8);
				laser_can_send(&frame);
			} else if (k_uptime_get() - latest_fw_up_times > 10000) {
				atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
				LOG_INF("Cancel Firmware upgrade status due to timeout 10s");
			}
		}
	}
}
K_THREAD_DEFINE(laser_msg, 2048, laser_msg_process_thread, NULL, NULL, NULL, 12, 0, 0);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_cb(const struct device *dev, void *user_data)
{
	static struct rx_buf buf;

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		buf.len += uart_fifo_read(dev, buf.data + buf.len, sizeof(buf.data) - buf.len);
		if (buf.len >= sizeof(buf.data)) {
			LOG_WRN("too much laser data");
			LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");
			memset(&buf, 0, sizeof(buf));
		} else if (buf.len > 2) {
			if (buf.data[buf.len - 1] == 0x0A && buf.data[buf.len - 2] == 0x0D) {
				if (enable_log) {
					LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");
				}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
				if (atomic_test_and_clear_bit(&laser_status, LASER_NEED_CLOSE)) {
					laser_stopclear();
				} else
#endif
				{
					k_msgq_put(&laser_serial_msgq, &buf, K_NO_WAIT);
				}
				memset(&buf, 0, sizeof(buf));
			} else if (buf.data[0] == 0xea) {
				memset(&buf, 0, sizeof(buf));
			}
		}
	}
}
#else
static void laser_uart_process_thread(void)
{
	static struct rx_buf buf = {0};
	int ret;

	while (true) {
		ret = uart_poll_in(uart_dev, buf.data + buf.len);
		if (ret == -1) {
			k_msleep(1);
			continue;
		}
		buf.len += 1;
		if (buf.len >= sizeof(buf.data)) {
			LOG_WRN("too much laser data");
			LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");
			memset(&buf, 0, sizeof(buf));
		} else if (buf.len > 2) {
			if (buf.data[buf.len-1] == 0x0A && buf.data[buf.len-2] == 0x0D) {
				if (enable_log)
					LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");

				k_msgq_put(&laser_serial_msgq, &buf, K_NO_WAIT);
				memset(&buf, 0, sizeof(buf));
			} else if (buf.data[0] == 0xea) {
				memset(&buf, 0, sizeof(buf));
			}
		}
	}
}
K_THREAD_DEFINE(laser_uart_poll, 2048, laser_uart_process_thread, NULL, NULL, NULL, 13, 0, 0);
#endif

static bool serial_send(const uint8_t *data, size_t len, uint32_t event)
{
	int ret = 0;

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	gpio_pin_set_dt(&rs485tx_gpios, 1);
#endif

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	k_busy_wait(1000);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
#endif
	if (enable_log)
		LOG_HEXDUMP_INF(data, len, "TX:");

	ret = k_event_wait(&laser_event, event, true, K_MSEC(1000));
	if ((ret & event) == 0) {
		LOG_ERR("serial without receive ack");
	}
	return (ret & event) == event;
}

int laser_stopclear(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dc\r\n", id);

	if (!serial_send(tx_buf, len, EVENT_LASER_STOP_ON)) return -1;
	atomic_clear_bit(&laser_status, LASER_ON);
	atomic_clear_bit(&laser_status, LASER_CON_MESURE);

	return 0;
}

int laser_on(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%do\r\n", id);

	if (!serial_send(tx_buf, len, EVENT_LASER_STOP_ON)) return -1;

	atomic_set_bit(&laser_status, LASER_ON);

	return 0;
}

static bool laser_read_error(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dre\r\n", id);

	return serial_send(tx_buf, len, EVENT_LASER_ERROR);
}

static bool laser_clear_error(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dce\r\n", id);

	return serial_send(tx_buf, len, EVENT_LASER_ERROR);
}

int laser_con_measure(uint32_t val)
{
	if (!laser_read_error()) return -1;
	if (!laser_clear_error()) return -1;

	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dh+%d\r\n", id, val);

	if (!serial_send(tx_buf, len, EVENT_LASER_MSG)) return -1;
	atomic_set_bit(&laser_status, LASER_CON_MESURE);
	atomic_set_bit(&laser_status, LASER_DEVICE_STATUS);
	return 0;
}

static int laser_setup(uint32_t val)
{
	// stop
	if (laser_stopclear()) return -1;
	// on
	if (laser_on()) return -1;

	if (laser_con_measure(val)) return -1;

	return 0;
}

static int laser_serial_init(void)
{
	struct uart_config uart_cfg = {.baudrate = 115200,
				       .parity = UART_CFG_PARITY_EVEN,
				       .stop_bits = UART_CFG_STOP_BITS_1,
				       .data_bits = UART_CFG_DATA_BITS_7,
				       .flow_ctrl = UART_CFG_FLOW_CTRL_NONE};

	int ret;
	if ((ret = uart_configure(uart_dev, &uart_cfg))) {
		if (ret != -ENOSYS && ret != -ENOTSUP) {
			LOG_ERR("uart config error: %d", ret);
			return -1;
		}
	}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	gpio_pin_configure_dt(&rs485tx_gpios, GPIO_OUTPUT_INACTIVE);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_set(uart_dev, uart_cb);
	uart_irq_rx_enable(uart_dev);
#endif

	return 0;
}

SYS_INIT(laser_serial_init, APPLICATION, 10);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_laser_status(const struct shell *ctx, size_t argc, char **argv)
{
	shell_print(ctx, "status: 0x%lx", laser_status);
	return 0;
}

static int cmd_laser_on(const struct shell *ctx, size_t argc, char **argv)
{
	if (laser_on()) {
		shell_error(ctx, "laser on error!");
		return -1;
	};
	return 0;
}

static int cmd_laser_stop(const struct shell *ctx, size_t argc, char **argv)
{
	if (laser_stopclear()) {
		shell_error(ctx, "laser stop error!");
		return -1;
	};
	return 0;
}

static int cmd_enable_log(const struct shell *ctx, size_t argc, char **argv)
{
	if (strcmp(argv[1], "on") == 0) {
		enable_log = true;
		shell_print(ctx, "Enable laser data dump");
	} else if (strcmp(argv[1], "off") == 0) {
		enable_log = false;
		shell_print(ctx, "Disable laser data dump");
	}
	return 0;
}

static int cmd_laser_mesure(const struct shell *ctx, size_t argc, char **argv)
{
	if (laser_con_measure(100)) {
		shell_error(ctx, "laser start measure error!");
		return -1;
	}
	return 0;
}

static int cmd_laser_write(const struct shell *ctx, size_t argc, char **argv)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "%s\r\n", argv[1]);

	serial_send(tx_buf, len, 0xf);

	return 0;
}

static int cmd_laser_setup(const struct shell *ctx, size_t argc, char **argv)
{
	if (laser_setup(strtoul(argv[1], NULL, 0))) {
		shell_error(ctx, "laser setup error!");
		return -1;
	};
	return 0;
}

static int cmd_laser_id(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(ctx, "laser id: %d", id);
		return 0;
	}
	id = (uint8_t)strtoul(argv[1], NULL, 0);
	shell_print(ctx, "set laser id: %d", id);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_serial_cmds,
			       SHELL_CMD_ARG(status, NULL,
					     "laser status\n"
					     "Usage: status",
					     cmd_laser_status, 1, 0),
			       SHELL_CMD_ARG(on, NULL,
					     "laser on\n"
					     "Usage: on",
					     cmd_laser_on, 1, 0),
			       SHELL_CMD_ARG(stop, NULL,
					     "laser stop\n"
					     "Usage: stop",
					     cmd_laser_stop, 1, 0),
			       SHELL_CMD_ARG(log, NULL,
					     "laser log\n"
					     "Usage: log <on/off>",
					     cmd_enable_log, 2, 0),
			       SHELL_CMD_ARG(mesure, NULL,
					     "laser mesure\n"
					     "Usage: mesure",
					     cmd_laser_mesure, 1, 0),
			       SHELL_CMD_ARG(send, NULL,
					     "serial send\n"
					     "Usage: send <strings>",
					     cmd_laser_write, 2, 0),
			       SHELL_CMD_ARG(setup, NULL,
					     "setup\n"
					     "Usage: setup <period>",
					     cmd_laser_setup, 2, 0),
			       SHELL_CMD_ARG(id, NULL,
					     "get/set laser id\n"
					     "Usage: id [<id>]",
					     cmd_laser_id, 1, 1),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(laser, &sub_serial_cmds, "laser commands", NULL);
#endif
