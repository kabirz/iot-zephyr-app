/*
 * USB CDC ACM echo example for the GD32H759 BTB board, using the
 * device_next (USBD) USB device stack.
 *
 * The USBHS0 controller enumerates as a CDC ACM device; everything the
 * host writes to the data interface is echoed back.  The on-board UART
 * shell (if enabled) stays available for debugging.
 *
 * From the development machine:
 *
 *   lsusb                       # 28e9:0575 EmbedFire GD32H759 BTB CDC ACM
 *   echo hello > /dev/ttyACM1   # the same bytes come back
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb_cdc_acm_echo, LOG_LEVEL_INF);

#define RING_BUF_SIZE 1024

static uint8_t ring_buffer[RING_BUF_SIZE];
static struct ring_buf ringbuf;
static bool rx_throttled;

static const struct device *const uart_dev =
	DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

/* ------------------------------------------------------------------ */
/* USB device context                                                  */
/* ------------------------------------------------------------------ */

USBD_DEVICE_DEFINE(cdc_acm_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_USB_CDC_ACM_EXAMPLE_VID,
		   CONFIG_USB_CDC_ACM_EXAMPLE_PID);

USBD_DESC_LANG_DEFINE(cdc_acm_lang);
USBD_DESC_MANUFACTURER_DEFINE(cdc_acm_mfr,
			      CONFIG_USB_CDC_ACM_EXAMPLE_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(cdc_acm_product,
			 CONFIG_USB_CDC_ACM_EXAMPLE_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(cdc_acm_sn);

USBD_DESC_CONFIG_DEFINE(cdc_acm_fs_cfg_desc, "FS Configuration");

static const uint8_t attributes =
	(IS_ENABLED(CONFIG_USB_CDC_ACM_EXAMPLE_SELF_POWERED) ?
	 USB_SCD_SELF_POWERED : 0) |
	(IS_ENABLED(CONFIG_USB_CDC_ACM_REMOTE_WAKEUP) ?
	 USB_SCD_REMOTE_WAKEUP : 0);

USBD_CONFIGURATION_DEFINE(cdc_acm_fs_config,
			  attributes,
			  CONFIG_USB_CDC_ACM_EXAMPLE_MAX_POWER,
			  &cdc_acm_fs_cfg_desc);

static void cdc_acm_msg_cb(struct usbd_context *const ctx,
			   const struct usbd_msg *const msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			LOG_INF("DTR set");
		}
	}
}

static int enable_usb_device(void)
{
	/* no classes on the blocklist */
	static const char *const blocklist[] = { NULL };
	int err;

	err = usbd_add_descriptor(&cdc_acm_usbd, &cdc_acm_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&cdc_acm_usbd, &cdc_acm_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&cdc_acm_usbd, &cdc_acm_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&cdc_acm_usbd, &cdc_acm_sn);
	if (err) {
		LOG_ERR("Failed to add serial number descriptor (%d)", err);
		return err;
	}

	err = usbd_add_configuration(&cdc_acm_usbd, USBD_SPEED_FS,
				     &cdc_acm_fs_config);
	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration (%d)", err);
		return err;
	}

	/* all CDC ACM instances declared in devicetree */
	err = usbd_register_all_classes(&cdc_acm_usbd, USBD_SPEED_FS, 1,
					blocklist);
	if (err) {
		LOG_ERR("Failed to register classes (%d)", err);
		return err;
	}

	/* CDC ACM is multi-interface, use the IAD class code triple */
	usbd_device_set_code_triple(&cdc_acm_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_self_powered(&cdc_acm_usbd,
			  attributes & USB_SCD_SELF_POWERED);

	err = usbd_msg_register_cb(&cdc_acm_usbd, cdc_acm_msg_cb);
	if (err) {
		LOG_ERR("Failed to register message callback (%d)", err);
		return err;
	}

	err = usbd_init(&cdc_acm_usbd);
	if (err) {
		LOG_ERR("Failed to initialize device support (%d)", err);
		return err;
	}

	err = usbd_enable(&cdc_acm_usbd);
	if (err) {
		LOG_ERR("Failed to enable device support (%d)", err);
		return err;
	}

	LOG_INF("USB device support enabled");

	return 0;
}

/* ------------------------------------------------------------------ */
/* CDC ACM echo                                                        */
/* ------------------------------------------------------------------ */

static void interrupt_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!rx_throttled && uart_irq_rx_ready(dev)) {
			uint8_t buffer[64];
			size_t len = MIN(ring_buf_space_get(&ringbuf),
					 sizeof(buffer));
			int recv_len, rb_len;

			if (len == 0) {
				/* throttle: ring buffer full */
				uart_irq_rx_disable(dev);
				rx_throttled = true;
				continue;
			}

			recv_len = uart_fifo_read(dev, buffer, len);
			if (recv_len < 0) {
				LOG_ERR("Failed to read UART FIFO");
				recv_len = 0;
			}

			rb_len = ring_buf_put(&ringbuf, buffer, recv_len);
			if (rb_len < recv_len) {
				LOG_ERR("Drop %u bytes", recv_len - rb_len);
			}

			if (rb_len) {
				uart_irq_tx_enable(dev);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t buffer[64];
			int rb_len, send_len;

			rb_len = ring_buf_get(&ringbuf, buffer, sizeof(buffer));
			if (!rb_len) {
				uart_irq_tx_disable(dev);
				continue;
			}

			if (rx_throttled) {
				uart_irq_rx_enable(dev);
				rx_throttled = false;
			}

			send_len = uart_fifo_fill(dev, buffer, rb_len);
			if (send_len < rb_len) {
				LOG_ERR("Drop %d bytes", rb_len - send_len);
			}
		}
	}
}

int main(void)
{
	uint32_t dtr = 0U;
	int ret;

	ring_buf_init(&ringbuf, sizeof(ring_buffer), ring_buffer);

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return -ENODEV;
	}

	ret = enable_usb_device();
	if (ret) {
		return ret;
	}

	/* host must open the port (DTR) before data flows */
	while (!dtr) {
		ret = uart_line_ctrl_get(uart_dev, UART_LINE_CTRL_DTR, &dtr);
		if (ret) {
			LOG_WRN("Failed to get DTR line state: %d", ret);
		}
		k_sleep(K_MSEC(100));
	}
	LOG_INF("Host connected (DTR set)");

	ret = uart_line_ctrl_get(uart_dev, UART_LINE_CTRL_BAUD_RATE, &dtr);
	if (ret == 0) {
		LOG_INF("Baudrate %u", dtr);
	}

	uart_irq_callback_set(uart_dev, interrupt_handler);
	uart_irq_rx_enable(uart_dev);

	LOG_INF("Echoing CDC ACM data, %u byte ring buffer", RING_BUF_SIZE);

	return 0;
}
