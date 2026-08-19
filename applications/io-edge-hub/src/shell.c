/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 应用调试 shell (根命令 io)
 *
 *   io                     -- 查看 IO/配置总览
 *   io info                -- 版本/MAC/IP/链路/RS485/CAN 基本信息
 *   io di                  -- DI1-16 状态
 *   io do / io do set n v  -- DO1-8 查看 / 单点控制 (FC05 同路径)
 *   io ai                  -- AI1-4 工程量 (0.01mA / 0.01V)
 *   io rs485               -- RS485 波特率 + Modbus slave id
 *   io rs485 baud n        -- 设波特率 (重启生效)
 *   io rs485 sid n         -- 设 slave id (重启生效)
 *   io can                 -- CAN ID + 波特率
 *   io can id n            -- 设业务帧 ID (重启生效)
 *   io can bps n           -- 设波特率 kbit/s (重启生效)
 *   io ip a.b.c.d          -- 设静态 IP (保存+重启生效)
 *   io reg [a [v]]         -- 寄存器全量 dump / 单读 / 单写 (FC03/FC06 同路径)
 *   io save                -- 参数持久化到 FCB
 *   io factory             -- 恢复出厂设置 (擦参数区 + 延迟重启)
 *
 * 写路径全部复用 io_write_holding / io_write_do_bit,
 * 与 Modbus/Web (HTTP/WS)/UDP 副作用一致。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/app_version.h>
#include <fw_gitver.h>
#include <zephyr/net/net_if.h>
#include "init.h"

/* 时间显示偏移: RTC 存 UTC, shell 按本地时区显示 (默认 UTC+8) */
#define IO_SHELL_TZ_OFFSET_SECS (8LL * 3600)

/* ==================== info ==================== */

static int cmd_info(const struct shell *sh, size_t argc, char *argv[])
{
	char mac_str[18] = "unknown";
	struct net_if *iface = net_if_get_default();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (iface != NULL) {
		const struct net_linkaddr *ll = net_if_get_link_addr(iface);

		if (ll->len == 6) {
			snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
				 ll->addr[0], ll->addr[1], ll->addr[2], ll->addr[3], ll->addr[4],
				 ll->addr[5]);
		}
	}

	shell_print(sh, "version : v%d.%d.%d_%s", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		    APP_PATCHLEVEL, FW_GIT_VERSION);
	shell_print(sh, "build   : %s %s", __DATE__, __TIME__);
	shell_print(sh, "board   : %s", CONFIG_BOARD_TARGET);
	shell_print(sh, "mac     : %s", mac_str);
	shell_print(sh, "ip      : %u.%u.%u.%u/24", get_holding_reg(HOLDING_IP_OCTET1_IDX),
		    get_holding_reg(HOLDING_IP_OCTET2_IDX), get_holding_reg(HOLDING_IP_OCTET3_IDX),
		    get_holding_reg(HOLDING_IP_OCTET4_IDX));
	shell_print(sh, "link    : %s", net_link_is_up() ? "up" : "down");
	shell_print(sh, "rs485   : %u bps, slave id %u (8N1)",
		    get_holding_reg(HOLDING_RS485_BAUDRATE_IDX),
		    get_holding_reg(HOLDING_SLAVE_ID_IDX));
	shell_print(sh, "can     : id 0x%03x, %u kbit/s", get_holding_reg(HOLDING_CAN_ID_IDX),
		    get_holding_reg(HOLDING_CAN_BAUDRATE_IDX));
	shell_print(sh, "uptime  : %lld s", (long long)(k_uptime_get() / 1000));
	/* RTC 存 UTC, 显示时加时区偏移 (Web 前端在浏览器侧做同样转换) */
	time_t now = time(NULL) + IO_SHELL_TZ_OFFSET_SECS;
	char time_str[20] = "1970-01-01 00:00:00";
	struct tm tm;

	gmtime_r(&now, &tm);
	if (tm.tm_year + 1900 >= 2020) {
		strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
	}
	shell_print(sh, "time    : %s (%lld)", time_str, (long long)time(NULL));
	return 0;
}

/* ==================== DI / DO / AI ==================== */

static int cmd_di(const struct shell *sh, size_t argc, char *argv[])
{
	uint16_t di = get_input_reg(INPUT_DI_IDX);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "DI: 0x%04x (enable: 0x%04x)", di, get_holding_reg(HOLDING_DI_ENABLE_IDX));
	for (int row = 0; row < DI_NUM; row += 8) {
		shell_fprintf(sh, SHELL_NORMAL, "DI%-2d-%-2d :", row + 1, row + 8);
		for (int i = 0; i < 8; i++) {
			shell_fprintf(sh, SHELL_NORMAL, " %u", (di >> (row + i)) & 1);
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}
	return 0;
}

static int cmd_do(const struct shell *sh, size_t argc, char *argv[])
{
	uint16_t do_v = get_holding_reg(HOLDING_DO_IDX);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "DO: 0x%02x", do_v & 0xFF);
	for (int row = 0; row < DO_NUM; row += 8) {
		shell_fprintf(sh, SHELL_NORMAL, "DO%-2d-%-2d :", row + 1, row + 8);
		for (int i = 0; i < 8; i++) {
			shell_fprintf(sh, SHELL_NORMAL, " %u", (do_v >> (row + i)) & 1);
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}
	return 0;
}

/* io do set <ch 1-8> <0|1> */
static int cmd_do_set(const struct shell *sh, size_t argc, char *argv[])
{
	long ch = strtol(argv[1], NULL, 0);
	long val = strtol(argv[2], NULL, 0);

	ARG_UNUSED(argc);

	if (ch < 1 || ch > DO_NUM) {
		shell_error(sh, "invalid channel: %s (1-%d)", argv[1], DO_NUM);
		return -EINVAL;
	}
	if (val != 0 && val != 1) {
		shell_error(sh, "invalid value: %s (0/1)", argv[2]);
		return -EINVAL;
	}
	if (io_write_do_bit((uint16_t)(ch - 1), val != 0) != 0) {
		shell_error(sh, "write failed");
		return -EIO;
	}
	shell_print(sh, "DO%ld = %ld (DO: 0x%02x)", ch, val,
		    get_holding_reg(HOLDING_DO_IDX) & 0xFF);
	return 0;
}

static int cmd_ai(const struct shell *sh, size_t argc, char *argv[])
{
	static const char *unit[AI_NUM] = {"mA", "mA", "V", "V"};
	uint16_t en = get_holding_reg(HOLDING_AI_ENABLE_IDX);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "AI enable: 0x%01x", en & 0x0F);
	for (int i = 0; i < AI_NUM; i++) {
		uint16_t raw = get_input_reg(INPUT_AI0_IDX + i);

		/* input_reg 存 0.01mA (AI1/2) / 0.01V (AI3/4), 展开为工程量 */
		shell_print(sh, "AI%d: %5u.%02u %s (raw %u)", i + 1, raw / 100, raw % 100, unit[i],
			    raw);
	}
	return 0;
}

/* ==================== RS485 / CAN / IP ==================== */

static int cmd_rs485(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rs485: %u bps, slave id %u (8N1)",
		    get_holding_reg(HOLDING_RS485_BAUDRATE_IDX),
		    get_holding_reg(HOLDING_SLAVE_ID_IDX));
	shell_print(sh, "(changes take effect after reboot)");
	return 0;
}

/* io rs485 baud <1200-115200> */
static int cmd_rs485_baud(const struct shell *sh, size_t argc, char *argv[])
{
	long v = strtol(argv[1], NULL, 0);

	ARG_UNUSED(argc);

	if (v < 1200 || v > 115200) {
		shell_error(sh, "invalid baud: %s (1200-115200)", argv[1]);
		return -EINVAL;
	}
	io_write_holding(HOLDING_RS485_BAUDRATE_IDX, (uint16_t)v);
	shell_print(sh, "rs485 baud -> %u (reboot to apply, 'io save' to persist)",
		    get_holding_reg(HOLDING_RS485_BAUDRATE_IDX));
	return 0;
}

/* io rs485 sid <1-247> */
static int cmd_rs485_sid(const struct shell *sh, size_t argc, char *argv[])
{
	long v = strtol(argv[1], NULL, 0);

	ARG_UNUSED(argc);

	if (v < 1 || v > 247) {
		shell_error(sh, "invalid slave id: %s (1-247)", argv[1]);
		return -EINVAL;
	}
	io_write_holding(HOLDING_SLAVE_ID_IDX, (uint16_t)v);
	shell_print(sh, "slave id -> %u (reboot to apply, 'io save' to persist)",
		    get_holding_reg(HOLDING_SLAVE_ID_IDX));
	return 0;
}

static int cmd_can(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "can: id 0x%03x, %u kbit/s", get_holding_reg(HOLDING_CAN_ID_IDX),
		    get_holding_reg(HOLDING_CAN_BAUDRATE_IDX));
	shell_print(sh, "(changes take effect after reboot)");
	return 0;
}

/* io can id <1-0x7FF> */
static int cmd_can_id(const struct shell *sh, size_t argc, char *argv[])
{
	long v = strtol(argv[1], NULL, 0);

	ARG_UNUSED(argc);

	if (v < 1 || v > 0x7FF) {
		shell_error(sh, "invalid can id: %s (1-0x7FF)", argv[1]);
		return -EINVAL;
	}
	io_write_holding(HOLDING_CAN_ID_IDX, (uint16_t)v);
	shell_print(sh, "can id -> 0x%03x (reboot to apply, 'io save' to persist)",
		    get_holding_reg(HOLDING_CAN_ID_IDX));
	return 0;
}

/* io can bps <50|100|125|250|500|800|1000> (kbit/s) */
static int cmd_can_bps(const struct shell *sh, size_t argc, char *argv[])
{
	long v = strtol(argv[1], NULL, 0);

	ARG_UNUSED(argc);

	switch (v) {
	case 50:
	case 100:
	case 125:
	case 250:
	case 500:
	case 800:
	case 1000:
		break;
	default:
		shell_error(sh, "invalid bps: %s (50/100/125/250/500/800/1000)", argv[1]);
		return -EINVAL;
	}
	io_write_holding(HOLDING_CAN_BAUDRATE_IDX, (uint16_t)v);
	shell_print(sh, "can bps -> %u kbit/s (reboot to apply, 'io save' to persist)",
		    get_holding_reg(HOLDING_CAN_BAUDRATE_IDX));
	return 0;
}

/* io ip <a.b.c.d> */
static int cmd_ip(const struct shell *sh, size_t argc, char *argv[])
{
	uint32_t a, b, c, d;

	ARG_UNUSED(argc);

	if (sscanf(argv[1], "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 ||
	    d > 255 || !ip_addr_valid((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d)) {
		shell_error(sh, "invalid ip: %s", argv[1]);
		return -EINVAL;
	}
	io_write_holding(HOLDING_IP_OCTET1_IDX, (uint16_t)a);
	io_write_holding(HOLDING_IP_OCTET2_IDX, (uint16_t)b);
	io_write_holding(HOLDING_IP_OCTET3_IDX, (uint16_t)c);
	io_write_holding(HOLDING_IP_OCTET4_IDX, (uint16_t)d);
	holding_reg_save();
	shell_print(sh, "ip -> %u.%u.%u.%u (saved, reboot to apply)", a, b, c, d);
	return 0;
}

/* ==================== 寄存器 (调试逃生口) ==================== */

/* io reg [addr [value]]: 无参全量 / 单读 / 单写 */
static int cmd_reg(const struct shell *sh, size_t argc, char *argv[])
{
	if (argc == 1) {
		shell_print(sh, "holding registers (%d):", CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS);
		for (int i = 0; i < CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%s0x%02x=%u",
				      (i % 6) ? " " : (i ? "\n" : ""), i, io_read_holding(i));
		}
		shell_fprintf(sh, SHELL_NORMAL, "\ninput registers (%d):\n",
			      CONFIG_MODBUS_INPUT_REGISTER_NUMBERS);
		for (int i = 0; i < CONFIG_MODBUS_INPUT_REGISTER_NUMBERS; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%s0x%02x=%u",
				      (i % 6) ? " " : (i ? "\n" : ""), i, get_input_reg(i));
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
		return 0;
	}

	char *end;
	long addr = strtol(argv[1], &end, 0);

	if (*end != '\0' || addr < 0 || addr >= CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS) {
		shell_error(sh, "invalid addr: %s", argv[1]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_print(sh, "holding[0x%02lx] = %u", addr, io_read_holding((uint16_t)addr));
		return 0;
	}

	long val = strtol(argv[2], &end, 0);

	if (*end != '\0' || val < 0 || val > 0xFFFF) {
		shell_error(sh, "invalid value: %s (0-65535)", argv[2]);
		return -EINVAL;
	}
	if (io_write_holding((uint16_t)addr, (uint16_t)val) != 0) {
		shell_error(sh, "write failed");
		return -EIO;
	}
	shell_print(sh, "holding[0x%02lx] = %u", addr, io_read_holding((uint16_t)addr));
	return 0;
}

/* ==================== save ==================== */

static int cmd_save(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	holding_reg_save();
	shell_print(sh, "parameters saved to FCB");
	return 0;
}

/* ==================== factory reset ==================== */

/* io factory: 擦 storage_partition (FCB 参数区), 置延迟重启
 * (main 循环 history_sync + 排空日志后冷重启, 与 UDP/Web 路径一致) */
static int cmd_factory(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (settings_factory_reset() != 0) {
		shell_error(sh, "factory reset failed (erase)");
		return -EIO;
	}
	set_reboot_status(true);
	shell_print(sh, "factory reset done, rebooting (defaults after reboot)");
	return 0;
}

/* ==================== 命令树 ==================== */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_io_do,
			       SHELL_CMD_ARG(set, NULL, "set DO output: <ch 1-8> <0|1>", cmd_do_set,
					     3, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_io_rs485,
			       SHELL_CMD_ARG(baud, NULL, "set baud <1200-115200> (reboot to apply)",
					     cmd_rs485_baud, 2, 0),
			       SHELL_CMD_ARG(sid, NULL,
					     "set modbus slave id <1-247> (reboot to apply)",
					     cmd_rs485_sid, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_io_can,
			       SHELL_CMD_ARG(id, NULL,
					     "set business frame id <1-0x7FF> (reboot to apply)",
					     cmd_can_id, 2, 0),
			       SHELL_CMD_ARG(bps, NULL,
					     "set baud <50/100/125/250/500/800/1000> kbit/s "
					     "(reboot to apply)",
					     cmd_can_bps, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_io, SHELL_CMD(info, NULL, "show version / mac / ip / rs485 / can info", cmd_info),
	SHELL_CMD(di, NULL, "show DI1-16 status", cmd_di),
	SHELL_CMD(do, &sub_io_do, "show DO1-8 status", cmd_do),
	SHELL_CMD(ai, NULL, "show AI1-4 values (mA / V)", cmd_ai),
	SHELL_CMD(rs485, &sub_io_rs485, "show rs485 baud / slave id", cmd_rs485),
	SHELL_CMD(can, &sub_io_can, "show can id / baud", cmd_can),
	SHELL_CMD_ARG(ip, NULL, "set static ip <a.b.c.d> (saved, reboot to apply)", cmd_ip, 2, 0),
	SHELL_CMD_ARG(reg, NULL,
		      "dump / read / write holding register: "
		      "[addr [value]]",
		      cmd_reg, 1, 2),
	SHELL_CMD(save, NULL, "persist parameters to FCB", cmd_save),
	SHELL_CMD(factory, NULL, "factory reset (erase params + reboot)", cmd_factory),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(io, &sub_io, "io-edge-hub debug commands", NULL);
