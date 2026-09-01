/*
 * USB CDC ACM echo endpoint.
 *
 * Enables the USB device stack and echoes everything received on the
 * CDC ACM data interface back to the host, so `lsusb` plus a simple
 * read/write loop measures end-to-end bring-up.  The UART shell stays
 * on USART0 for board-side debugging.  Modeled on the legacy
 * samples/subsys/usb/legacy/cdc_acm sample.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(usb_cdc_echo, LOG_LEVEL_INF);

static const struct device *const cdc_dev =
	DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

#define RING_BUF_SIZE 1024

static uint8_t ring_buffer[RING_BUF_SIZE];
static struct ring_buf ringbuf;
static bool rx_throttled;

static void interrupt_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!rx_throttled && uart_irq_rx_ready(dev)) {
			uint8_t buffer[64];
			int recv_len, rb_len;
			size_t len = MIN(ring_buf_space_get(&ringbuf),
					 sizeof(buffer));

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

static void usb_cdc_thread(void *p1, void *p2, void *p3)
{
	uint32_t dtr = 0U;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sleep(K_SECONDS(1));

	ring_buf_init(&ringbuf, sizeof(ring_buffer), ring_buffer);

	ret = usb_enable(NULL);
	if (ret) {
		LOG_ERR("usb_enable failed: %d", ret);
		return;
	}
	LOG_INF("USB device enabled");

	if (!device_is_ready(cdc_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return;
	}

	/* host must open the port (DTR) before data flows */
	while (!dtr) {
		ret = uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
		if (ret) {
			LOG_ERR("Failed to get DTR line state: %d", ret);
		}
		k_sleep(K_MSEC(100));
	}
	LOG_INF("DTR set, host connected");

	/* optional: assert DCD/DSR to exercise the control endpoint */
	(void)uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DCD, 1);
	(void)uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DSR, 1);
	k_msleep(100);

	uart_irq_callback_set(cdc_dev, interrupt_handler);
	uart_irq_rx_enable(cdc_dev);

	while (true) {
		k_sleep(K_SECONDS(60));
	}
}

K_THREAD_DEFINE(usb_cdc_echo_thr, 2048, usb_cdc_thread, NULL, NULL, NULL,
		5, 0, 0);
