/*
 * MDIO 总线调试 shell 命令 (bring-up 用):
 *
 *   mdio scan                 扫描地址 0..31, 列出应答的 PHY 及其 ID
 *   mdio read <addr> <reg>    读 PHY 寄存器
 *   mdio write <addr> <reg> <val>  写 PHY 寄存器
 *
 * 若 DTS 里 ethernet-phy@N 的 N 配错, PHY 驱动初始化会报
 * "not a YT8512" / "MDIO read failed", 用 mdio scan 找到实际地址后
 * 改 board DTS 的 reg 即可。
 *
 * 注意: 与 PHY 驱动的周期链路监控并发访问同一条 MDIO 总线,
 * 仅用于调试, 不保证读写原子性。
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/mii.h>
#include <errno.h>
#include <stdlib.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(mdio), okay)

static const struct device *const mdio_dev = DEVICE_DT_GET(DT_NODELABEL(mdio));

static int cmd_mdio_scan(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int found = 0;

	shell_print(ctx, "scanning MDIO bus %s ...", mdio_dev->name);

	for (uint8_t addr = 0; addr < 32; addr++) {
		uint16_t id1 = 0, id2 = 0;

		if (mdio_read(mdio_dev, addr, MII_PHYID1R, &id1) < 0 ||
		    mdio_read(mdio_dev, addr, MII_PHYID2R, &id2) < 0) {
			continue;
		}
		/* 总线无应答读回全 1, 全 0 也不是有效 PHY */
		if ((id1 == 0xFFFFU && id2 == 0xFFFFU) ||
		    (id1 == 0x0000U && id2 == 0x0000U)) {
			continue;
		}

		uint16_t bmsr = 0;

		(void)mdio_read(mdio_dev, addr, MII_BMSR, &bmsr);
		shell_print(ctx,
			    "PHY @0x%02X: ID 0x%04X.%04X (YT8512 Type_No=0x12), "
			    "BMSR 0x%04X%s%s",
			    addr, id1, id2, bmsr,
			    (bmsr & MII_BMSR_LINK_STATUS) ? ", link up" : "",
			    (bmsr & MII_BMSR_AUTONEG_COMPLETE) ? ", autoneg done" : "");
		found++;
	}

	if (found == 0) {
		shell_error(ctx, "no PHY responded on the MDIO bus");
		return -ENODEV;
	}

	return 0;
}

static int cmd_mdio_read(const struct shell *ctx, size_t argc, char **argv)
{
	uint8_t addr = (uint8_t)strtol(argv[1], NULL, 0);
	uint8_t reg = (uint8_t)strtol(argv[2], NULL, 0);
	uint16_t val = 0;

	if (mdio_read(mdio_dev, addr, reg, &val) < 0) {
		shell_error(ctx, "mdio read %s failed: %d", mdio_dev->name, errno);
		return -EIO;
	}

	shell_print(ctx, "phy 0x%02X reg 0x%02X = 0x%04X", addr, reg, val);
	return 0;
}

static int cmd_mdio_write(const struct shell *ctx, size_t argc, char **argv)
{
	uint8_t addr = (uint8_t)strtol(argv[1], NULL, 0);
	uint8_t reg = (uint8_t)strtol(argv[2], NULL, 0);
	uint16_t val = (uint16_t)strtol(argv[3], NULL, 0);

	if (mdio_write(mdio_dev, addr, reg, val) < 0) {
		shell_error(ctx, "mdio write %s failed: %d", mdio_dev->name, errno);
		return -EIO;
	}

	shell_print(ctx, "phy 0x%02X reg 0x%02X <- 0x%04X", addr, reg, val);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_mdio_cmds,
	SHELL_CMD_ARG(scan, NULL, "scan MDIO bus for PHYs (addr 0..31)", cmd_mdio_scan, 1, 0),
	SHELL_CMD_ARG(read, NULL, "read PHY register: mdio read <addr> <reg>",
		      cmd_mdio_read, 3, 0),
	SHELL_CMD_ARG(write, NULL, "write PHY register: mdio write <addr> <reg> <val>",
		      cmd_mdio_write, 4, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(mdio, &sub_mdio_cmds, "MDIO bus debug tools", NULL);

#else /* no mdio node */
#warning "board has no enabled mdio node; mdio shell commands are stubs"

static int cmd_mdio_none(const struct shell *ctx, size_t argc, char **argv)
{
	shell_error(ctx, "board has no enabled mdio node in devicetree");
	return -ENODEV;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_mdio_cmds,
	SHELL_CMD_ARG(scan, NULL, "scan MDIO bus for PHYs", cmd_mdio_none, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(mdio, &sub_mdio_cmds, "MDIO bus debug tools", NULL);

#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(mdio), okay) */
