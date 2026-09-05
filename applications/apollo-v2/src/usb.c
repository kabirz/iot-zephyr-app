/* SPDX-License-Identifier: Apache-2.0
 * USB 变体: OTG FS 枚举为 CDC ACM 虚拟串口 (device_next 栈)。
 * 控制台仍在 USART1; CDC 数据口供后续 shell 透传/数据通道扩展。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

#include "version.h"

LOG_MODULE_REGISTER(apollo_usb, LOG_LEVEL_INF);

USBD_DEVICE_DEFINE(apollo_usbd,
		   DEVICE_DT_GET(DT_CHOSEN(zephyr_udc0)),
		   CONFIG_APOLLO_USB_VID,
		   CONFIG_APOLLO_USB_PID);

USBD_DESC_LANG_DEFINE(apollo_lang);
USBD_DESC_MANUFACTURER_DEFINE(apollo_mfr, CONFIG_APOLLO_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(apollo_product, CONFIG_APOLLO_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(apollo_sn);

USBD_DESC_CONFIG_DEFINE(apollo_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(apollo_fs_config,
			  0,	/* bus powered */
			  100,	/* 100 mA */
			  &apollo_fs_cfg_desc);

static void apollo_usbd_msg_cb(struct usbd_context *const ctx,
			       const struct usbd_msg *const msg)
{
	LOG_INF("USBD: %s", usbd_msg_type_string(msg->type));

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		LOG_INF("CDC ACM DTR %s", dtr ? "set" : "clear");
	}
}

void apollo_usb_start(void)
{
	static const char *const blocklist[] = { NULL };
	int err;

	err = usbd_add_descriptor(&apollo_usbd, &apollo_lang);
	if (err) {
		LOG_ERR("lang descriptor: %d", err);
		return;
	}
	err = usbd_add_descriptor(&apollo_usbd, &apollo_mfr);
	if (err) {
		LOG_ERR("mfr descriptor: %d", err);
		return;
	}
	err = usbd_add_descriptor(&apollo_usbd, &apollo_product);
	if (err) {
		LOG_ERR("product descriptor: %d", err);
		return;
	}
	err = usbd_add_descriptor(&apollo_usbd, &apollo_sn);
	if (err) {
		LOG_ERR("serial descriptor: %d", err);
		return;
	}
	err = usbd_add_configuration(&apollo_usbd, USBD_SPEED_FS, &apollo_fs_config);
	if (err) {
		LOG_ERR("add FS configuration: %d", err);
		return;
	}
	err = usbd_register_all_classes(&apollo_usbd, USBD_SPEED_FS, 1, blocklist);
	if (err) {
		LOG_ERR("register classes: %d", err);
		return;
	}
	/* CDC ACM 是多接口类, 用 IAD 类码三元组 */
	usbd_device_set_code_triple(&apollo_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_msg_register_cb(&apollo_usbd, apollo_usbd_msg_cb);

	err = usbd_init(&apollo_usbd);
	if (err) {
		LOG_ERR("usbd_init: %d", err);
		return;
	}
	err = usbd_enable(&apollo_usbd);
	if (err) {
		LOG_ERR("usbd_enable: %d", err);
		return;
	}

	LOG_INF("USB CDC ACM 枚举完成 (%04x:%04x)",
		(unsigned)CONFIG_APOLLO_USB_VID, (unsigned)CONFIG_APOLLO_USB_PID);
}
