#include "laser-common.h"
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_flash, LOG_LEVEL_INF);

#ifdef CONFIG_BOARD_NATIVE_SIM
#define CFG_PARTITION    storage_partition
#else
#define CFG_PARTITION    cfg_partition
#endif
#define CFG_PARTITION_ID PARTITION_ID(CFG_PARTITION)

static const struct flash_area *fa;
static uint32_t laser_regs[50];
bool inited;

static int laser_flash_init(void)
{
	if (flash_area_open(CFG_PARTITION_ID, &fa) != 0) {
		LOG_ERR("flash open failed");
		inited = false;
		return -1;
	}

	if (flash_area_read(fa, 0, laser_regs, sizeof(laser_regs)) != 0) {
		flash_area_close(fa);
		LOG_ERR("flash read failed");
		fa = NULL;
		return -1;
	}
	inited = true;
	return 0;
}

int laser_flash_read_mode(void)
{
	if (!inited) {
		if (laser_flash_init()) return -1;
	}
	atomic_clear_bit(&laser_status, LASER_WRITE_MODE);
	return 0;
}

int laser_flash_write_mode(void)
{
	if (!inited) {
		if (laser_flash_init()) return -1;
	}

	atomic_set_bit(&laser_status, LASER_WRITE_MODE);
	return 0;
}

int laser_flash_write(uint16_t address, uint32_t val)
{
	if (!atomic_test_bit(&laser_status, LASER_WRITE_MODE) ||
		atomic_test_bit(&laser_status, LASER_FW_UPDATE)) {
		LOG_ERR("Please enable write mode first");
		return -1;
	}
	if (address >= 50) {
		LOG_ERR("only support 50 regs");
		return -1;
	}
	if (laser_regs[address] != val) {
		laser_regs[address] = val;
		if (flash_area_flatten(fa, 0, 4096) == 0) {
			if (flash_area_write(fa, 0, laser_regs, sizeof(laser_regs)) == 0) {
				return 0;
			}
		}
	} else return 0;
	LOG_ERR("flash write failed");

	return -1;
}

int laser_flash_read(uint16_t address, uint32_t *val)
{
	if (!inited) {
		LOG_ERR("%s: flash error", __FUNCTION__);
		return -1;
	}
	if (address >= 50) {
		LOG_ERR("only support 50 regs");
		return -1;
	}
	*val = laser_regs[address];
	return 0;
}

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <stdlib.h>
static int cmd_lflash_mode(const struct shell *ctx, size_t argc, char **argv)
{
	if (argv[1][0] == '0' && argv[1][1] == '\0') {
		laser_flash_read_mode();
	} else if (argv[1][0] == '1' && argv[1][1] == '\0') {
		laser_flash_write_mode();
	} else {
		shell_print(ctx, "Usage: lfash mode 0 or lfash mode 1");
		return -1;
	}
	return 0;
}

static int cmd_lflash_write(const struct shell *ctx, size_t argc, char **argv)
{
	uint32_t l_buf;
	uint8_t regs = strtol(argv[1], NULL, 0);

	for (int i = 0; i < argc - 2; i++) {
		l_buf = strtol(argv[2 + i], NULL, 0);
		laser_flash_write(regs+i, l_buf);
	}

	return 0;
}

static int cmd_lflash_read(const struct shell *ctx, size_t argc, char **argv)
{
	uint8_t regs = strtol(argv[1], NULL, 0);
	uint32_t val;

	for (int i = 0; i < strtol(argv[2], NULL, 0); i++) {
		laser_flash_read(regs+i, &val);
		shell_print(ctx, "%d: 0x%x", regs+i, val);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lflash_cmds,
			       SHELL_CMD_ARG(mode, NULL,
					     "set flash mode\n"
					     "Usage: mode <0/1> , 0: read mode, 1: write mode",
					     cmd_lflash_mode, 2, 0),
			       SHELL_CMD_ARG(write, NULL,
					     "write bytes to flash\n"
					     "Usage: write <reg> <word1> [<word2>..], reg < 256",
					     cmd_lflash_write, 3, 7),
			       SHELL_CMD_ARG(read, NULL,
					     "read bytes from flash\n"
					     "Usage: read <reg> <numbers>",
					     cmd_lflash_read, 3, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lflash, &sub_lflash_cmds, "Spi Flash commands", NULL);
#endif
