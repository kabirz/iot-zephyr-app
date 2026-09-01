/*
 * GD32H7xx USBHS register dump and bring-up rehearsal.
 *
 * Direct register access (no HAL): turns on the PMU 3.3V USB supply, the
 * IRC48M PHY reference clock and the USBHS0 bus gate, then dumps the DWC2
 * core identification and hardware configuration registers.  `gdusb fs`
 * selects the embedded FS PHY and runs a core soft reset to verify the
 * clocking path before the real driver integration.
 *
 * Memory map (vendor gd32h73x_75x.h): APB4 0x58000000 (PMU at +0x5800),
 * AHB1 0x40020000 (USBHS0 at +0x20000), AHB4 0x58020000 (RCU at +0x4400).
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#define RCU_BASE        0x58024400UL
#define RCU_AHB1EN      (*reg32(RCU_BASE, 0x30))
#define RCU_APB4EN      (*reg32(RCU_BASE, 0x4c))
#define RCU_ADDCTL0     (*reg32(RCU_BASE, 0xc0))
#define RCU_USBCLKCTL   (*reg32(RCU_BASE, 0xd4))

#define PMU_BASE        0x58005800UL
#define PMU_CTL2        (*reg32(PMU_BASE, 0x10))

#define USBHS0_BASE     0x40040000UL
#define USBHS1_BASE     0x40080000UL

/* DWC2 global register offsets (GigaDevice names: GUSBCS/GCCFG/GOTGCS) */
#define GOTGCTL_OFF     0x00
#define GUSBCS_OFF      0x0c
#define GRSTCTL_OFF     0x10
#define GCCFG_OFF       0x38
#define GSNPSID_OFF     0x40
#define GHWCFG1_OFF     0x44
#define GHWCFG2_OFF     0x48
#define GHWCFG3_OFF     0x4c
#define GHWCFG4_OFF     0x50

/* GRSTCTL */
#define GRSTCTL_CSFTRST BIT(0)
#define GRSTCTL_AHBIDLE BIT(31)

/* GUSBCS bit 6: embedded full-speed PHY select (GigaDevice EMBPHY_FS) */
#define GUSBCS_EMBPHY_FS BIT(6)

static inline volatile uint32_t *reg32(uintptr_t base, uintptr_t off)
{
	return (volatile uint32_t *)(base + off);
}

static int usb_clocks_up(const struct shell *sh)
{
	int t;

	/* PMU: enable the 3.3V USB supply regulator and its voltage
	 * detector, wait (bounded) for the supply-ready flag. */
	shell_print(sh, "[1] PMU regulator...");
	RCU_APB4EN |= BIT(4);              /* PMU bus clock */
	PMU_CTL2 |= (BIT(25) | BIT(24));   /* USBSEN + VUSB33DEN */
	for (t = 0; !(PMU_CTL2 & BIT(26)) && t < 100000; t++) {
		k_busy_wait(1);            /* USB33RF */
	}
	if (!(PMU_CTL2 & BIT(26))) {
		shell_print(sh, "  TIMEOUT: USB33RF never set, PMU_CTL2=0x%08x",
			    PMU_CTL2);
		return -ETIMEDOUT;
	}
	shell_print(sh, "  ready, PMU_CTL2=0x%08x", PMU_CTL2);

	/* 48MHz PHY reference from IRC48M */
	shell_print(sh, "[2] IRC48M...");
	RCU_ADDCTL0 |= BIT(16);            /* IRC48MEN */
	for (t = 0; !(RCU_ADDCTL0 & BIT(17)) && t < 100000; t++) {
		k_busy_wait(1);            /* IRC48MSTB */
	}
	if (!(RCU_ADDCTL0 & BIT(17))) {
		shell_print(sh, "  TIMEOUT: IRC48MSTB, ADDCTL0=0x%08x",
			    RCU_ADDCTL0);
		return -ETIMEDOUT;
	}
	RCU_USBCLKCTL = (RCU_USBCLKCTL & ~(3U << 5)) | (3U << 5);
	shell_print(sh, "  ready, USBCLKCTL=0x%08x", RCU_USBCLKCTL);

	/* AHB bus gate for USBHS0 */
	shell_print(sh, "[3] RCU AHB gate...");
	RCU_AHB1EN |= BIT(14);
	k_busy_wait(10);
	shell_print(sh, "  AHB1EN=0x%08x", RCU_AHB1EN);

	return 0;
}

static void dump_core(const struct shell *sh, uintptr_t base, const char *name)
{
	uint32_t g2 = *reg32(base, GHWCFG2_OFF);
	uint32_t g3 = *reg32(base, GHWCFG3_OFF);
	uint32_t g4 = *reg32(base, GHWCFG4_OFF);
	int i;

	shell_print(sh, "---- %s @ 0x%08x ----", name, base);
	shell_print(sh, "  GSNPSID=0x%08x GUSBCS=0x%08x GCCFG=0x%08x GOTGCTL=0x%08x",
		    *reg32(base, GSNPSID_OFF), *reg32(base, GUSBCS_OFF),
		    *reg32(base, GCCFG_OFF), *reg32(base, GOTGCTL_OFF));
	shell_print(sh, "  GHWCFG1=0x%08x GHWCFG2=0x%08x GHWCFG3=0x%08x GHWCFG4=0x%08x",
		    *reg32(base, GHWCFG1_OFF), g2, g3, g4);
	shell_print(sh, "  decoded: num_dev_eps=%u num_in_eps=%u ded_fifo=%u fifo_dwords=%u",
		    (g2 >> 10) & 0xF, (g4 >> 26) & 0xF, (g4 >> 25) & 1, g3 & 0xFFFF);

	/* raw sweep of the global register file so unlisted (SVD-gap)
	 * registers show up too */
	for (i = 0; i < 0x64; i += 16) {
		shell_print(sh,
			    "  0x%02x: %08x %08x %08x %08x",
			    i,
			    *reg32(base, i), *reg32(base, i + 4),
			    *reg32(base, i + 8), *reg32(base, i + 12));
	}
	/* device domain: DCFG@0x800 DCTL@0x804 DSTS@0x808 DAINT@0x818 */
	shell_print(sh, "  DCFG=0x%08x DCTL=0x%08x DSTS=0x%08x DAINT=0x%08x",
		    *reg32(base, 0x800), *reg32(base, 0x804),
		    *reg32(base, 0x808), *reg32(base, 0x818));
	shell_print(sh, "  DIEP0: ctl=0x%08x int=0x%08x tsiz=0x%08x",
		    *reg32(base, 0x900), *reg32(base, 0x908),
		    *reg32(base, 0x910));
}

static int cmd_gdusb_dump(const struct shell *sh, size_t argc, char **argv)
{
	int idx = (argc > 1) ? atoi(argv[1]) : 0;
	int ret;

	if (idx == 1) {
		/* deliberately risky: USBHS1 may not be bonded on LQFP144,
		 * an unmapped AHB read faults the kernel */
		RCU_AHB1EN |= BIT(29);
		k_busy_wait(10);
		dump_core(sh, USBHS1_BASE, "USBHS1");
		return 0;
	}

	ret = usb_clocks_up(sh);
	if (ret) {
		return ret;
	}

	dump_core(sh, USBHS0_BASE, "USBHS0");
	shell_print(sh, "  done.");

	return 0;
}

static int cmd_gdusb_fs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uintptr_t base = USBHS0_BASE;
	volatile uint32_t *grstctl = reg32(base, GRSTCTL_OFF);
	int t, ret;

	ret = usb_clocks_up(sh);
	if (ret) {
		return ret;
	}

	shell_print(sh, "[4] FS PHY select...");
	*reg32(base, GUSBCS_OFF) |= GUSBCS_EMBPHY_FS;
	k_busy_wait(10);

	if (!(*grstctl & GRSTCTL_AHBIDLE)) {
		shell_print(sh, "  AHB idle never set: GRSTCTL=0x%08x",
			    *grstctl);
		return -EIO;
	}

	shell_print(sh, "[5] core soft reset...");
	*grstctl |= GRSTCTL_CSFTRST;
	for (t = 0; (*grstctl & GRSTCTL_CSFTRST) && t < 100000; t++) {
		k_busy_wait(1);
	}
	if (*grstctl & GRSTCTL_CSFTRST) {
		shell_print(sh, "  TIMEOUT: CSFTRST, GRSTCTL=0x%08x",
			    *grstctl);
		return -ETIMEDOUT;
	}
	k_busy_wait(100);

	shell_print(sh, "  OK: GUSBCS=0x%08x GRSTCTL=0x%08x",
		    *reg32(base, GUSBCS_OFF), *grstctl);

	return 0;
}

static int cmd_gdusb_pwron(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret;

	ret = usb_clocks_up(sh);
	if (ret) {
		return ret;
	}

	/* power the FS transceiver, let it settle, then re-dump: the
	 * ID/config registers live in the PHY clock domain */
	shell_print(sh, "[4] GCCFG.PWRON...");
	*reg32(USBHS0_BASE, GCCFG_OFF) |= BIT(16);
	k_msleep(20);

	dump_core(sh, USBHS0_BASE, "USBHS0 (PHY powered)");

	return 0;
}

static int cmd_gdusb_dev(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	volatile uint32_t *dcfg = reg32(USBHS0_BASE, 0x800);
	volatile uint32_t *dctl = reg32(USBHS0_BASE, 0x804);

	/* probe: do writes into the device-clock-domain register file
	 * stick?  DCFG@0x800, DCTL@0x804 */
	shell_print(sh, "before: DCFG=0x%08x DCTL=0x%08x", *dcfg, *dctl);
	*dcfg = 0x12345678;
	*dctl = 0x55aa55aa;
	k_busy_wait(10);
	shell_print(sh, "after write: DCFG=0x%08x DCTL=0x%08x", *dcfg, *dctl);
	*dcfg = 0;
	*dctl = 0;
	k_busy_wait(10);
	shell_print(sh, "after clear: DCFG=0x%08x DCTL=0x%08x", *dcfg, *dctl);

	return 0;
}

static int cmd_gdusb_connect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	volatile uint32_t *dctl = reg32(USBHS0_BASE, 0x804);
	int arg = (argc > 1) ? atoi(argv[1]) : 1;

	/* set (arg 1) or clear (arg 0) SFTDISCON (bit 1) manually */
	if (arg) {
		*dctl |= BIT(1);
	} else {
		*dctl &= ~BIT(1);
	}
	k_busy_wait(10);
	shell_print(sh, "DCTL=0x%08x", *dctl);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(gdusb_cmds,
	SHELL_CMD(dump, NULL, "dump USBHS0 DWC2 core regs (arg: 1 = USBHS1, may fault)",
		  cmd_gdusb_dump),
	SHELL_CMD(pwron, NULL, "power FS transceiver + dump", cmd_gdusb_pwron),
	SHELL_CMD(fs, NULL, "select embedded FS PHY + core soft reset (USBHS0)",
		  cmd_gdusb_fs),
	SHELL_CMD(dev, NULL, "probe device-domain register writes", cmd_gdusb_dev),
	SHELL_CMD(connect, NULL, "set/clear SFTDISCON (arg: 0 = connect)",
		  cmd_gdusb_connect),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gdusb, &gdusb_cmds, "GD32 USBHS debug", NULL);
