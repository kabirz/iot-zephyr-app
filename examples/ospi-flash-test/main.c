/*
 * Copyright (c) 2026 GD32H7xx Zephyr bring-up
 * SPDX-License-Identifier: Apache-2.0
 *
 * OSPI0 on-board GD25Q64 NOR flash self-test: JEDEC ID check at boot
 * (via the driver init log), a 4KB-sector erase / page-program /
 * read-back round trip, and a memory-mapped (quad fast read) test
 * with an indirect-vs-mapped read performance comparison.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/gd32_ospi.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/cache.h>
#include <string.h>

#include <gd32_ospi.h>

LOG_MODULE_REGISTER(ospi_flash_test, LOG_LEVEL_INF);

#define FLASH_NODE DT_NODELABEL(ospi0)
static const struct device *const flash_dev = DEVICE_DT_GET(FLASH_NODE);

/* memory-mapped window from the board devicetree */
#define MM_BASE  DT_REG_ADDR(DT_NODELABEL(ospi0_mm))

/* scratch sector: last 4KB of the 8MB flash, out of any data area */
#define TEST_OFFSET 0x7ff000U
#define TEST_LEN    4096U
/* block used for the read performance comparison (last 64KB) */
#define PERF_OFFSET 0x7f0000U
#define PERF_LEN    0x10000U

static uint8_t wbuf[TEST_LEN];
static uint8_t rbuf[TEST_LEN];
static uint8_t p1[PERF_LEN];
static uint8_t p2[PERF_LEN];

static int run_self_test(void)
{
	int ret;

	if (!device_is_ready(flash_dev)) {
		LOG_ERR("ospi0 flash device not ready");
		return -ENODEV;
	}

	for (size_t i = 0; i < TEST_LEN; i++) {
		wbuf[i] = (uint8_t)(i ^ 0xa5);
	}

	ret = flash_erase(flash_dev, TEST_OFFSET, TEST_LEN);
	if (ret != 0) {
		LOG_ERR("erase failed: %d", ret);
		return ret;
	}

	ret = flash_read(flash_dev, TEST_OFFSET, rbuf, TEST_LEN);
	if (ret != 0) {
		LOG_ERR("read-after-erase failed: %d", ret);
		return ret;
	}
	for (size_t i = 0; i < TEST_LEN; i++) {
		if (rbuf[i] != 0xff) {
			LOG_ERR("erase verify failed at +%zu: 0x%02x",
				i, rbuf[i]);
			return -EIO;
		}
	}

	ret = flash_write(flash_dev, TEST_OFFSET, wbuf, TEST_LEN);
	if (ret != 0) {
		LOG_ERR("write failed: %d", ret);
		return ret;
	}

	ret = flash_read(flash_dev, TEST_OFFSET, rbuf, TEST_LEN);
	if (ret != 0) {
		LOG_ERR("read failed: %d", ret);
		return ret;
	}
	if (memcmp(rbuf, wbuf, TEST_LEN) != 0) {
		for (size_t i = 0; i < TEST_LEN; i++) {
			if (rbuf[i] != wbuf[i]) {
				LOG_ERR("verify failed at +%zu: wrote 0x%02x read 0x%02x",
					i, wbuf[i], rbuf[i]);
				break;
			}
		}
		return -EIO;
	}

	LOG_INF("OSPI FLASH OK: sector 0x%08x erase + %u bytes write/readback verified",
		TEST_OFFSET, TEST_LEN);

	return 0;
}

static int run_mm_test(void)
{
	volatile const uint8_t *mm;
	uintptr_t base = 0;
	uint64_t t0, t1, cycles_hz = sys_clock_hw_cycles_per_sec();
	uint32_t ind_us, mm_us;
	int ret;

	/* The MM window is a non-cacheable MPU region (see the board dts):
	 * the OSPI AHB slave locks up on D-cache line-fill (wrapping
	 * burst) reads, so the data cache must stay off while the OSPI
	 * driver is used. */

	/* reference data through the indirect path */
	ret = flash_read(flash_dev, PERF_OFFSET, p1, PERF_LEN);
	if (ret != 0) {
		LOG_ERR("indirect read failed: %d", ret);
		return ret;
	}

	ret = flash_gd32_ospi_mm_enable(flash_dev, &base);
	if (ret != 0) {
		LOG_ERR("mm enable failed: %d", ret);
		return ret;
	}
	if (base != MM_BASE) {
		LOG_ERR("mm base 0x%lx != dt 0x%x", (unsigned long)base, MM_BASE);
		flash_gd32_ospi_mm_disable(flash_dev);
		return -EINVAL;
	}

	/* window is non-cacheable: no cache maintenance needed */

	/* mapped read of the same block */
	mm = (volatile const uint8_t *)(base + PERF_OFFSET);
	t0 = k_cycle_get_64();
	memcpy(p2, (const void *)mm, PERF_LEN);
	t1 = k_cycle_get_64();
	mm_us = (uint32_t)((t1 - t0) * 1000000ULL / cycles_hz);

	/* one-shot spot check of a single byte load */
	uint8_t b = mm[TEST_OFFSET - PERF_OFFSET];

	if (memcmp(p1, p2, PERF_LEN) != 0 || b != wbuf[0]) {
		LOG_ERR("memory-mapped read data mismatch");
		flash_gd32_ospi_mm_disable(flash_dev);
		return -EIO;
	}

	/* indirect timing for comparison */
	t0 = k_cycle_get_64();
	ret = flash_read(flash_dev, PERF_OFFSET, p1, PERF_LEN);
	t1 = k_cycle_get_64();
	ind_us = (uint32_t)((t1 - t0) * 1000000ULL / cycles_hz);
	if (ret != 0) {
		LOG_ERR("indirect read failed: %d", ret);
		return ret;
	}

	LOG_INF("MM OK: %uKB indirect %u us (%u KB/s) vs mapped %u us (%u KB/s), data identical",
		PERF_LEN / 1024, ind_us, PERF_LEN / 1024U * 1000000U / MAX(ind_us, 1U),
		mm_us, PERF_LEN / 1024U * 1000000U / MAX(mm_us, 1U));

	/* flash API ops must transparently leave memory-mapped mode */
	ret = flash_gd32_ospi_mm_disable(flash_dev);
	if (ret != 0) {
		return ret;
	}

	return run_self_test();
}

static int cmd_flashtest(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = run_self_test();

	shell_print(sh, "self-test %s (%d)", ret == 0 ? "PASSED" : "FAILED", ret);

	return ret;
}
SHELL_CMD_REGISTER(flashtest, NULL, "re-run the OSPI flash self-test", cmd_flashtest);

static int cmd_mmtest(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = run_mm_test();

	shell_print(sh, "mm-test %s (%d)", ret == 0 ? "PASSED" : "FAILED", ret);

	return ret;
}
SHELL_CMD_REGISTER(mmtest, NULL, "memory-mapped read test + performance comparison", cmd_mmtest);

/* direct JEDEC ID read (0x9F) through the vendor OSPI API, for
 * interactive verification outside of the lost early-boot log window */
static int cmd_flashid(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ospi_parameter_struct ospi_cfg = {0};
	ospi_regular_cmd_struct cmd = {0};
	uint8_t id[3] = {0};

	cmd.operation_type = OSPI_OPTYPE_COMMON_CFG;
	cmd.instruction = 0x9fU;
	cmd.ins_mode = OSPI_INSTRUCTION_1_LINE;
	cmd.ins_size = OSPI_INSTRUCTION_8_BITS;
	cmd.addr_mode = OSPI_ADDRESS_NONE;
	cmd.addr_size = OSPI_ADDRESS_24_BITS;
	cmd.addr_dtr_mode = OSPI_ADDRDTR_MODE_DISABLE;
	cmd.alter_bytes_mode = OSPI_ALTERNATE_BYTES_NONE;
	cmd.alter_bytes_size = OSPI_ALTERNATE_BYTES_24_BITS;
	cmd.alter_bytes_dtr_mode = OSPI_ABDTR_MODE_DISABLE;
	cmd.data_mode = OSPI_DATA_1_LINE;
	cmd.data_dtr_mode = OSPI_DADTR_MODE_DISABLE;
	cmd.dummy_cycles = OSPI_DUMYC_CYCLES_0;
	cmd.nbdata = 3;

	ospi_command_config(OSPI0, &ospi_cfg, &cmd);
	ospi_receive(OSPI0, id);

	shell_print(sh, "jedec id: %02x %02x %02x (expect c8 40 17)",
		    id[0], id[1], id[2]);

	return 0;
}
SHELL_CMD_REGISTER(flashid, NULL, "read the flash JEDEC ID over OSPI", cmd_flashid);

/* dump the ARMv7-M MPU register state for debugging the MM window */
static int cmd_mpudump(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	volatile uint32_t *mpu_type = (uint32_t *)0xE000ED90U;
	volatile uint32_t *mpu_ctrl = (uint32_t *)0xE000ED94U;
	volatile uint32_t *mpu_rnr = (uint32_t *)0xE000ED98U;
	volatile uint32_t *mpu_rbar = (uint32_t *)0xE000ED9CU;
	volatile uint32_t *mpu_rasr = (uint32_t *)0xE000EDA0U;

	shell_print(sh, "MPU regions=%u ctrl=0x%08x (ENABLE=%u PRIVDEFENA=%u)",
		    (*mpu_type >> 8) & 0xff, *mpu_ctrl,
		    (*mpu_ctrl >> 0) & 1, (*mpu_ctrl >> 2) & 1);

	for (uint32_t i = 0; i < 12; i++) {
		uint32_t rbar, rasr, tex, c, b, s, sz;

		*mpu_rnr = i;
		rbar = *mpu_rbar;
		rasr = *mpu_rasr;
		if ((rasr & 1U) == 0U) {
			continue;
		}
		tex = (rasr >> 19) & 7U;
		s = (rasr >> 18) & 1U;
		c = (rasr >> 17) & 1U;
		b = (rasr >> 16) & 1U;
		sz = (rasr >> 1) & 0x1fU;
		shell_print(sh, "  [%u] base=0x%08x size=%u  TEX=%u S=%u C=%u B=%u XN=%u",
			    i, rbar & ~0x1fU, 1U << (sz + 1U), tex, s, c, b,
			    rasr & 1U);
	}

	return 0;
}
SHELL_CMD_REGISTER(mpudump, NULL, "dump MPU regions", cmd_mpudump);

int main(void)
{
	LOG_INF("ospi-flash-test starting");
	/* let the DAPLink CDC settle after reset so boot logs survive */
	k_sleep(K_SECONDS(1));
	(void)run_self_test();

	return 0;
}
