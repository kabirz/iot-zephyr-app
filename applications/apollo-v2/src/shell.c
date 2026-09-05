/* SPDX-License-Identifier: Apache-2.0
 * 应用 shell (根命令 apollo):
 *   apollo info                        板/版本/运行状态汇总
 *   apollo beep [ms]                   蜂鸣器
 *   apollo led <0|1> <on|off|toggle>   LED 控制
 *   apollo adc                         ADC1 PA5 采样
 *   apollo time                        当前本地时间
 *   apollo timeset YYYY-MM-DD HH:MM:SS 设 RTC (本地时间, UTC+8)
 *   apollo cansend <id> <hex...>       (CAN 变体) 发送标准帧
 *   apollo reboot                      重启
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/disk_access.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"
#include "board_io.h"
#include "app_time.h"
#include "storage.h"
#include "net.h"
#ifdef CONFIG_APOLLO_CAN
#include <zephyr/drivers/can.h>	/* CAN_MAX_DLEN (与本地 can.h 同名, 需显式引入) */
#include "can.h"
#endif

/* main.c: MCUboot 镜像确认 */
extern int apollo_fw_confirm(void);

/* QSPI W25Q256 扇区擦除 (救援用): apollo qspie <hex-offset> <hex-len> */
static int cmd_qspie(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor));
	uint32_t off, len;

	if (argc < 3) {
		shell_error(sh, "usage: apollo qspie <hex-offset> <hex-len>");
		return -EINVAL;
	}
	off = strtoul(argv[1], NULL, 16);
	len = strtoul(argv[2], NULL, 16);
	if (!device_is_ready(dev)) {
		shell_error(sh, "qspi not ready");
		return -ENODEV;
	}
	int ret = flash_erase(dev, off, len);

	if (ret != 0) {
		shell_error(sh, "erase failed: %d", ret);
		return ret;
	}
	shell_print(sh, "erased 0x%x + 0x%x", off, len);
	return 0;
}

/* TF 卡扇区读取诊断: apollo sdump <dec-lba> (hexdump 前 96 字节) */
static int cmd_sdump(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t buf[512];

	if (argc < 2) {
		shell_error(sh, "usage: apollo sdump <dec-lba>");
		return -EINVAL;
	}
	uint32_t lba = strtoul(argv[1], NULL, 10);
	int ret = disk_access_read("SD", buf, lba, 1);

	if (ret != 0) {
		shell_error(sh, "read lba %u failed: %d", lba, ret);
		return ret;
	}
	for (int row = 0; row < 6; row++) {
		shell_print(sh, "%08x: %02x %02x %02x %02x %02x %02x %02x %02x "
			      "%02x %02x %02x %02x %02x %02x %02x %02x",
			    lba * 512 + row * 16,
			    buf[row * 16 + 0], buf[row * 16 + 1],
			    buf[row * 16 + 2], buf[row * 16 + 3],
			    buf[row * 16 + 4], buf[row * 16 + 5],
			    buf[row * 16 + 6], buf[row * 16 + 7],
			    buf[row * 16 + 8], buf[row * 16 + 9],
			    buf[row * 16 + 10], buf[row * 16 + 11],
			    buf[row * 16 + 12], buf[row * 16 + 13],
			    buf[row * 16 + 14], buf[row * 16 + 15]);
	}
	/* MBR 分区表高亮 (LBA0 的 0x1BE 起) */
	if (lba == 0) {
		uint8_t *p = &buf[0x1BE];

		for (int i = 0; i < 4; i++, p += 16) {
			if (p[4] != 0) {
				uint32_t start = p[8] | (p[9] << 8) | (p[10] << 16) | (p[11] << 24);
				uint32_t size = p[12] | (p[13] << 8) | (p[14] << 16) | (p[15] << 24);

				shell_print(sh, "part%d: type=0x%02x start_lba=%u sectors=%u",
					    i + 1, p[4], start, size);
			}
		}
	}
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	const struct apollo_storage_info *si = storage_info();
	uint32_t key_cnt[4];
	int adc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "apollo-v2 %s (built " __DATE__ " " __TIME__ ")",
		   APOLLO_FW_VERSION);
	shell_print(sh, "uptime   %u s", k_uptime_get_32() / 1000);
	shell_print(sh, "time     %s", app_time_local_str());
	adc = board_io_adc_mv();
	if (adc >= 0) {
		shell_print(sh, "adc PA5   %d mV", adc);
	}
	for (size_t i = 0; i < 4; i++) {
		if (board_io_key_count(i, &key_cnt[i])) {
			shell_print(sh, "key%zu     %u", i, key_cnt[i]);
		}
	}
	shell_print(sh, "qspi     %s", si->qspi_ok ? "ok" : "n/a");
	shell_print(sh, "sdram    %s", si->sdram_ok ? "ok" : "n/a");
	shell_print(sh, "tf card  %s", si->sd_mounted ? "/SD: mounted" : "not mounted");
	shell_print(sh, "ipv4     %s%s", net_ip_str() ? net_ip_str() : "(none)",
		    net_ip_source() == NET_IP_DHCP ? " (dhcp)" :
		    net_ip_source() == NET_IP_STATIC_FALLBACK ? " (static fallback)" : "");
#ifdef CONFIG_APOLLO_CAN
	if (can_is_up()) {
		uint32_t rx, tx;

		can_get_stats(&rx, &tx);
		shell_print(sh, "can      up, rx %u tx %u", rx, tx);
	} else {
		shell_print(sh, "can      down");
	}
#endif
	return 0;
}

static int cmd_beep(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t ms = 100;

	if (argc >= 2) {
		ms = (uint16_t)CLAMP(strtoul(argv[1], NULL, 10), 10, 2000);
	}
	shell_print(sh, "beep %u ms", ms);
	return board_io_beep(ms);
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	unsigned idx;
	int ret;

	if (argc < 3) {
		shell_error(sh, "usage: apollo led <0|1> <on|off|toggle>");
		return -EINVAL;
	}
	idx = (unsigned)strtoul(argv[1], NULL, 10);
	if (strcmp(argv[2], "on") == 0) {
		ret = board_io_led_set(idx, true);
	} else if (strcmp(argv[2], "off") == 0) {
		ret = board_io_led_set(idx, false);
	} else if (strcmp(argv[2], "toggle") == 0) {
		ret = board_io_led_toggle(idx);
	} else {
		shell_error(sh, "unknown action '%s'", argv[2]);
		return -EINVAL;
	}
	return ret;
}

static int cmd_adc(const struct shell *sh, size_t argc, char **argv)
{
	int mv = board_io_adc_mv();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (mv < 0) {
		shell_error(sh, "adc failed: %d", mv);
		return mv;
	}
	shell_print(sh, "PA5 = %d mV", mv);
	return 0;
}

static int cmd_time(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "local (UTC+8): %s", app_time_local_str());
	return 0;
}

static int cmd_timeset(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "usage: apollo timeset YYYY-MM-DD HH:MM:SS");
		return -EINVAL;
	}
	int ret = app_time_set_local_str(argv[1], argv[2]);

	if (ret != 0) {
		shell_error(sh, "invalid time or RTC write failed: %d", ret);
		return ret;
	}
	shell_print(sh, "RTC set, local now: %s", app_time_local_str());
	return 0;
}

#ifdef CONFIG_APOLLO_CAN
static int cmd_cansend(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t data[CAN_MAX_DLEN];
	uint32_t id;
	size_t len = 0;

	if (argc < 3) {
		shell_error(sh, "usage: apollo cansend <hex-id> <hex-bytes...>");
		return -EINVAL;
	}
	id = strtoul(argv[1], NULL, 16);
	if (id > 0x7FF) {
		shell_error(sh, "standard id overflow: 0x%x", id);
		return -EINVAL;
	}
	for (size_t i = 2; i < argc && len < sizeof(data); i++) {
		char byte_hex[3] = { argv[i][0], argv[i][1], 0 };
		long v = strtol(byte_hex, NULL, 16);

		if (argv[i][0] == '\0' || argv[i][1] == '\0' || v < 0) {
			shell_error(sh, "bad hex byte '%s'", argv[i]);
			return -EINVAL;
		}
		data[len++] = (uint8_t)v;
	}
	int ret = can_send_frame(id, data, len);

	if (ret != 0) {
		shell_error(sh, "can send failed: %d", ret);
		return ret;
	}
	shell_print(sh, "sent id=0x%03x len=%u", id, (unsigned)len);
	return 0;
}
#endif /* CONFIG_APOLLO_CAN */

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sys_reboot(SYS_REBOOT_WARM);
	return 0;
}

#ifdef CONFIG_BOOTLOADER_MCUBOOT
static int cmd_confirm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = apollo_fw_confirm();

	if (ret == 0) {
		shell_print(sh, "image confirmed");
	}
	return ret;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_apollo,
	SHELL_CMD(info, NULL, "board/fw/runtime summary", cmd_info),
	SHELL_CMD_ARG(beep, NULL, "beep [ms=100]", cmd_beep, 1, 1),
	SHELL_CMD_ARG(led, NULL, "led <0|1> <on|off|toggle>", cmd_led, 3, 0),
	SHELL_CMD(adc, NULL, "read ADC1 PA5 (mV)", cmd_adc),
	SHELL_CMD(time, NULL, "show local time (UTC+8)", cmd_time),
	SHELL_CMD_ARG(timeset, NULL, "timeset YYYY-MM-DD HH:MM:SS (local)",
		      cmd_timeset, 3, 0),
#ifdef CONFIG_APOLLO_CAN
	SHELL_CMD_ARG(cansend, NULL, "cansend <hex-id> <hex-byte>...",
		      cmd_cansend, 3, 8),
#endif
	SHELL_CMD(reboot, NULL, "warm reboot", cmd_reboot),
#ifdef CONFIG_BOOTLOADER_MCUBOOT
	SHELL_CMD(confirm, NULL, "confirm current MCUboot image", cmd_confirm),
#endif
	SHELL_CMD_ARG(qspie, NULL, "qspie <hex-offset> <hex-len> (QSPI erase)",
		      cmd_qspie, 3, 0),
	SHELL_CMD_ARG(sdump, NULL, "sdump <dec-lba> (TF sector hexdump)",
		      cmd_sdump, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(apollo, &sub_apollo, "apollo-v2 application commands", NULL);
