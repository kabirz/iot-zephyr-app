/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus 寄存器管理 + Settings 持久化 (直接映射 holding_reg[])
 *
 * holding_reg[] / input_reg[] 是唯一的参数与采样数据源:
 *   - settings (FCB, modbus/ 命名空间) 直接映射 holding_reg[] 元素
 *   - DI/AI 采样线程写入 input_reg[]
 *   - Modbus holding 写 (FC06/FC16) 经 holding_reg_wr 回调产生副作用
 *     (DO 输出 / 历史开关 / 设置时间 / 参数保存 / 重启)
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/app_version.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>
#include <init.h>

LOG_MODULE_REGISTER(io_function, LOG_LEVEL_INF);

/* 寄存器并发保护: holding_reg[] 单字读写对齐时原子, 但 coil 的"读-改-写"
 * (FC05/15 写单个 DO 位) 与 holding_reg_save() 的全量导出读存在并发:
 *  - Modbus TCP/RTU 的 coil_wr_cb (系统工作队列) 与 UDP handler 的 update
 *    并发写 DO 位会丢失更新;
 *  - CFG_SAVE 回调或 UDP handler 调 holding_reg_save() 期间, 并发 update_holding_reg
 *    会让持久化到半更新状态 (如 IP 只写了前 2 字节)。
 * 用一把互斥锁覆盖这两类临界区 (单字 update/read 本身原子, 不加锁)。 */
static K_MUTEX_DEFINE(reg_lock);

/* ==================== 寄存器数组 (唯一数据源) ==================== */
static uint16_t holding_reg[CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS] = {
	[HOLDING_DI_ENABLE_IDX]	= 0xFFFF,	/* DI 全使能 */
	[HOLDING_AI_ENABLE_IDX]	= 0x000F,	/* AI 全使能 */
	[HOLDING_DI_SAMPLE_MS_IDX]	= 200,		/* DI 采样间隔 ms */
	[HOLDING_AI_SAMPLE_MS_IDX]	= 200,		/* AI 采样间隔 ms */
	[HOLDING_CAN_ID_IDX]	= 0x0111,	/* CAN ID */
	[HOLDING_CAN_BAUDRATE_IDX]	= 250,		/* CAN 波特率 x1000, 与 CONFIG_CAN_FW_UPGRADE_BITRATE 一致 */
	[HOLDING_RS485_BAUDRATE_IDX]	= 9600,		/* RS485 波特率 */
	[HOLDING_SLAVE_ID_IDX]	= 1,		/* Modbus Slave ID */
	[HOLDING_IP_OCTET1_IDX]	= 192,		/* 默认 IP 192.168.12.101 */
	[HOLDING_IP_OCTET2_IDX]	= 168,
	[HOLDING_IP_OCTET3_IDX]	= 12,
	[HOLDING_IP_OCTET4_IDX]	= 101,
};

static uint16_t input_reg[CONFIG_MODBUS_INPUT_REGISTER_NUMBERS] = {
	/* 主/次版本 <16, 三段塞进 16 位: MAJOR<<12 | MINOR<<8 | PATCH */
	[INPUT_VER_IDX] = ((APP_VERSION_MAJOR << 12) | (APP_VERSION_MINOR << 8) |
			   APP_PATCHLEVEL),
};

/* ==================== 寄存器访问接口 ==================== */
uint16_t get_holding_reg(uint16_t addr)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return 0;
	}
	return holding_reg[addr];
}

/* 读 holding 寄存器 (与 Modbus FC03 读回调同语义): 时间戳寄存器
 * (0x0E/0x0F) 返回实时系统时间, 其余返回数组值。供 Web /api/regs
 * 使用, 保证与 Modbus 主站读到的值一致 */
uint16_t io_read_holding(uint16_t addr)
{
	if (addr == HOLDING_TIMESTAMP_HI_IDX) {
		return (uint16_t)((uint32_t)time(NULL) >> 16);
	}
	if (addr == HOLDING_TIMESTAMP_LO_IDX) {
		return (uint16_t)(uint32_t)time(NULL);
	}
	return get_holding_reg(addr);
}

/* 内部设值 (无副作用), 供采样/UDP handler 使用 */
int update_holding_reg(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -ENOTSUP;
	}
	holding_reg[addr] = reg;
	return 0;
}

uint16_t get_input_reg(uint16_t addr)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return 0;
	}
	return input_reg[addr];
}

int update_input_reg(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return -ENOTSUP;
	}
	input_reg[addr] = reg;
	return 0;
}

/* 触发全量保存 (供 UDP handler 改参数后持久化)。
 * 加锁: 防止 export 读全量 holding_reg 期间, 其他线程并发 update 写入导致
 * 持久化到半更新状态。注意 CFG_SAVE 写回调也调 settings_save, 但那条路径
 * 跑在 wr_cb 里不会与本锁递归 (wr_cb 不持锁)。 */
void holding_reg_save(void)
{
	k_mutex_lock(&reg_lock, K_FOREVER);
	settings_save();
	k_mutex_unlock(&reg_lock);
}

/* ==================== Modbus 用户回调 (FC01/02/03/04/05/06/15/16) ==================== */

static int holding_reg_rd_cb(uint16_t addr, uint16_t *reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -ENOTSUP;
	}
	/* 时间戳寄存器读时返回实时系统时间 (而非数组里的陈旧值).
	 * 注意: 设备首次上电若 RTC 未初始化 (LSI 默认), time(NULL) 可能返回 0,
	 * 客户端读到 0x0000_0000 属正常, 设置时间后即正常. */
	if (addr == HOLDING_TIMESTAMP_HI_IDX) {
		*reg = (uint16_t)((uint32_t)time(NULL) >> 16);
		return 0;
	}
	if (addr == HOLDING_TIMESTAMP_LO_IDX) {
		*reg = (uint16_t)(uint32_t)time(NULL);
		return 0;
	}
	*reg = holding_reg[addr];
	return 0;
}

/* 写 holding 寄存器 (带副作用, 与 Modbus FC06/FC16 语义一致)。
 * 供 Modbus 写回调与 Web (HTTP/WS) 共用, 保证所有写入路径行为一致。 */
int io_write_holding(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -ENOTSUP;
	}

	holding_reg[addr] = reg;

	switch (addr) {
	case HOLDING_DO_IDX:
		/* DO 输出 + LED 联动 */
		mb_set_do(reg & 0xFF);
		break;
	case HOLDING_SLAVE_ID_IDX:
		/* RTU/TCP server 的 unit_id 在启动时固定, 需重启生效 */
		LOG_WRN("slave_id change requires reboot");
		break;
	case HOLDING_HISTORY_ENABLE_IDX:
		history_enable_write(reg != 0);
		break;
	case HOLDING_TIMESTAMP_LO_IDX:
		/* 写低16位时, 组合高低位设置 RTC 时间 */
		set_timestamp((time_t)(((uint32_t)holding_reg[HOLDING_TIMESTAMP_HI_IDX] << 16) |
				       reg));
		break;
	case HOLDING_CONFIG_SAVE_IDX:
		/* 写非0 → 全量保存参数到 FCB, 然后恢复为 0 */
		holding_reg[addr] = 0;
		settings_save();
		break;
	case HOLDING_REBOOT_IDX:
		if (reg) {
			sys_reboot(SYS_REBOOT_COLD);
		}
		break;
	default:
		break;
	}
	return 0;
}

static int holding_reg_wr_cb(uint16_t addr, uint16_t reg)
{
	return io_write_holding(addr, reg);
}

static int input_reg_rd_cb(uint16_t addr, uint16_t *reg)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return -ENOTSUP;
	}
	*reg = input_reg[addr];
	return 0;
}

/* Coil (FC01/05) 映射到 holding_reg[DO_IDX] 的位 */
static int coil_rd_cb(uint16_t addr, bool *state)
{
	if (addr >= DO_NUM) {
		return -ENOTSUP;
	}
	*state = (holding_reg[HOLDING_DO_IDX] & BIT(addr)) != 0;
	return 0;
}

/* 单 DO 位写 (加锁读-改-写, 与 Modbus FC05 语义一致), 供 Web (HTTP/WS) 共用 */
int io_write_do_bit(uint16_t bit, bool state)
{
	uint16_t val;

	if (bit >= DO_NUM) {
		return -ENOTSUP;
	}
	k_mutex_lock(&reg_lock, K_FOREVER);
	val = holding_reg[HOLDING_DO_IDX];
	WRITE_BIT(val, bit, state);
	mb_set_do(val & 0xFF);
	holding_reg[HOLDING_DO_IDX] = val & 0xFF;
	k_mutex_unlock(&reg_lock);
	return 0;
}

static int coil_wr_cb(uint16_t addr, bool state)
{
	return io_write_do_bit(addr, state);
}

/* Discrete Input (FC02) 映射到 input_reg[DI_IDX] 的位 */
static int discrete_input_rd_cb(uint16_t addr, bool *state)
{
	if (addr >= DI_NUM) {
		return -ENOTSUP;
	}
	*state = (input_reg[INPUT_DI_IDX] & BIT(addr)) != 0;
	return 0;
}

/* Modbus 用户回调表 (init.c 注册给 modbus_init_server) */
const struct modbus_user_callbacks io_modbus_cbs = {
	.holding_reg_rd = holding_reg_rd_cb,
	.holding_reg_wr = holding_reg_wr_cb,
	.input_reg_rd = input_reg_rd_cb,
	.coil_rd = coil_rd_cb,
	.coil_wr = coil_wr_cb,
	.discrete_input_rd = discrete_input_rd_cb,
};

/* ==================== Settings 持久化 (FCB, modbus/ 命名空间) ==================== */

/* Settings 键名精确匹配: 防止 "di_enx" 之类更长的段名误匹配 */
#define NAME_IS(n, l, s)	((l) == (sizeof(s) - 1) && !strncmp((n), (s), (l)))

static int mb_set_one(const char *name, size_t len, settings_read_cb read_cb,
		      void *cb_arg, uint16_t addr)
{
	if (len == sizeof(uint16_t)) {
		uint16_t val;

		if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
			holding_reg[addr] = val;
		}
	}
	return 0;
}

static int mb_handle_set(const char *name, size_t len, settings_read_cb read_cb,
			 void *cb_arg)
{
	const char *next;
	size_t name_len = settings_name_next(name, &next);

	/* IP 地址为 8B (4x uint16_t) */
	if (!next && NAME_IS(name, name_len, "ip")) {
		if (len == sizeof(uint16_t) * 4) {
			uint16_t ip[4];

			if (read_cb(cb_arg, ip, sizeof(ip)) == sizeof(ip)) {
				for (int i = 0; i < 4; i++) {
					holding_reg[HOLDING_IP_OCTET1_IDX + i] = ip[i];
				}
			}
		}
		return 0;
	}

	if (next) {
		return -ENOENT;
	}

	if (NAME_IS(name, name_len, "di_en")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_DI_ENABLE_IDX);
	}
	if (NAME_IS(name, name_len, "ai_en")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_AI_ENABLE_IDX);
	}
	if (NAME_IS(name, name_len, "di_si")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_DI_SAMPLE_MS_IDX);
	}
	if (NAME_IS(name, name_len, "ai_si")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_AI_SAMPLE_MS_IDX);
	}
	if (NAME_IS(name, name_len, "his")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_HISTORY_ENABLE_IDX);
	}
	if (NAME_IS(name, name_len, "can_id")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_CAN_ID_IDX);
	}
	if (NAME_IS(name, name_len, "can_bps")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_CAN_BAUDRATE_IDX);
	}
	if (NAME_IS(name, name_len, "rs485_bps")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_RS485_BAUDRATE_IDX);
	}
	if (NAME_IS(name, name_len, "slave_id")) {
		return mb_set_one(name, len, read_cb, cb_arg, HOLDING_SLAVE_ID_IDX);
	}

	return -ENOENT;
}

/* IP 合法性: 末字节非 0/0xff, 首字节非 0/127/组播(224-239)/保留(>=240) */
bool ip_addr_valid(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	if (d == 0 || d == 0xFF) {
		return false;
	}
	if (a == 0 || a == 127 || a >= 224) {
		return false;
	}
	return true;
}

/* 导出前校验 holding_reg 中的 IP */
static bool ip_is_valid_for_export(void)
{
	return ip_addr_valid((uint8_t)holding_reg[HOLDING_IP_OCTET1_IDX],
			     (uint8_t)holding_reg[HOLDING_IP_OCTET2_IDX],
			     (uint8_t)holding_reg[HOLDING_IP_OCTET3_IDX],
			     (uint8_t)holding_reg[HOLDING_IP_OCTET4_IDX]);
}

static int mb_handle_export(int (*cb)(const char *name, const void *value,
				      size_t val_len))
{
	(void)cb("modbus/di_en", &holding_reg[HOLDING_DI_ENABLE_IDX], sizeof(uint16_t));
	(void)cb("modbus/ai_en", &holding_reg[HOLDING_AI_ENABLE_IDX], sizeof(uint16_t));
	(void)cb("modbus/di_si", &holding_reg[HOLDING_DI_SAMPLE_MS_IDX], sizeof(uint16_t));
	(void)cb("modbus/ai_si", &holding_reg[HOLDING_AI_SAMPLE_MS_IDX], sizeof(uint16_t));
	(void)cb("modbus/his", &holding_reg[HOLDING_HISTORY_ENABLE_IDX], sizeof(uint16_t));
	(void)cb("modbus/can_id", &holding_reg[HOLDING_CAN_ID_IDX], sizeof(uint16_t));
	(void)cb("modbus/can_bps", &holding_reg[HOLDING_CAN_BAUDRATE_IDX], sizeof(uint16_t));
	(void)cb("modbus/rs485_bps", &holding_reg[HOLDING_RS485_BAUDRATE_IDX], sizeof(uint16_t));
	(void)cb("modbus/slave_id", &holding_reg[HOLDING_SLAVE_ID_IDX], sizeof(uint16_t));
	if (ip_is_valid_for_export()) {
		(void)cb("modbus/ip", &holding_reg[HOLDING_IP_OCTET1_IDX],
			 sizeof(uint16_t) * 4);
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(modbus, "modbus", NULL, mb_handle_set, NULL,
			       mb_handle_export);

/* ==================== 出厂恢复 ==================== */
int settings_factory_reset(void)
{
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &fa);

	if (rc == 0) {
		rc = flash_area_erase(fa, 0, fa->fa_size);
		flash_area_close(fa);
	}
	if (rc != 0) {
		LOG_ERR("factory reset erase failed: %d", rc);
		return rc;
	}

	LOG_INF("factory reset done, rebooting");
	return 0;
}
