/*
 * TFTLCD (FMC 8080 16 位并口) 测试, shell 命令 `lcd`:
 *   1. MPU IO 属性区 (0x60000000/64MB 禁缓存) 已在板 DTS 声明
 *      (lcd_mem 节点, zephyr,memory-attr = DT_MEM_ARM_MPU_IO),
 *      ARM MPU 驱动启动时静态建立 —— M7 默认把该区当可缓存内存,
 *      写会滞留 D-Cache, 屏幕收不到任何数据
 *   2. 探测控制器 ID (ILI9341 0xD3 / NT35510 0xD000 等)
 *   3. ILI9341 (240x320) 初始化 + 8 色彩条; 背光 PB5
 *
 * 板 DTS: FMC NE1 (PD7) @0x60000000, RS=PD13 (FMC_A18):
 *   RS=0 -> 寄存器 0x60000000, RS=1 -> 数据 0x60080000
 *   (16 位总线: FMC_A18 对应 HADDR bit19, 偏移 0x80000)
 *
 * 排查记录 (对照 /home/zhp/tftlcd.zip ATK 例程 + 寄存器级 dump 对比):
 *   - BCR1 bit31 为正常现象 (ATK 工作配置同样置位), 与写保护无关
 *   - WFDIS(bit21)/EXTMOD/CCLKEN 均非关键, 寄存器值与 ATK 逐位一致
 *     仍不亮, 最终定位为 MPU 区域被 Zephyr 栈保护动态清除
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(lcd, LOG_LEVEL_INF);

#define LCD_REG_BASE		((volatile uint16_t *)0x60000000)

/* RS(FMC_A18) 的字节偏移候选: 16 位总线时 A18 = HADDR bit19 */
static const uint32_t rs_offs[] = { 0x80000, 0x40000, 0x100000 };

static volatile uint16_t *lcd_data;
static uint16_t lcd_w, lcd_h;

static void lcd_cmd(uint16_t c)
{
	LCD_REG_BASE[0] = c;
}

static void lcd_dat(uint16_t d)
{
	lcd_data[0] = d;
}

static uint16_t lcd_rd(void)
{
	return lcd_data[0];
}

static void lcd_write_reg(uint16_t c, const uint16_t *params, int n)
{
	lcd_cmd(c);
	for (int i = 0; i < n; i++) {
		lcd_dat(params[i]);
	}
}

/* 探测: 对每个 RS 偏移读 ID, 返回 0 识别到已知控制器 */
static int lcd_probe(const struct shell *sh)
{
	static const uint16_t probe_cmds[] = { 0xD3, 0xD000, 0xA1 };

	for (size_t o = 0; o < ARRAY_SIZE(rs_offs); o++) {
		lcd_data = (volatile uint16_t *)(0x60000000 + rs_offs[o]);

		for (size_t c = 0; c < ARRAY_SIZE(probe_cmds); c++) {
			uint16_t r[4] = { 0, 0, 0, 0 };

			lcd_cmd(probe_cmds[c]);
			k_usleep(20);
			for (int i = 0; i < 4; i++) {
				r[i] = lcd_rd();
				k_usleep(5);
			}

			if (r[0] == 0xFFFF || (r[0] | r[1] | r[2] | r[3]) == 0) {
				continue;	/* 无应答 */
			}

			if (r[2] == 0x93 && r[3] == 0x41) {
				shell_print(sh, "detected ILI9341 (240x320)");
				lcd_w = 240;
				lcd_h = 320;
				return 0;
			}
			if (r[0] == 0x55 && r[1] == 0x10) {
				shell_print(sh, "detected NT35510 (480x800)");
				lcd_w = 480;
				lcd_h = 800;
				return 0;
			}
			if (r[0] == 0x57 && r[1] == 0x61) {
				shell_print(sh, "detected SSD1963");
				return -ENOTSUP;
			}
		}
	}
	shell_error(sh, "no LCD controller responded (check 排线/供电)");
	return -ENODEV;
}

static void ili9341_init(void)
{
	static const uint16_t pwr1[] = { 0x23 };
	static const uint16_t pwr2[] = { 0x10 };
	static const uint16_t vmctrl1[] = { 0x3E, 0x28 };
	static const uint16_t vmctrl2[] = { 0x86 };
	static const uint16_t madctl[] = { 0x48 };	/* MX | BGR */
	static const uint16_t pixfmt[] = { 0x55 };	/* RGB565 */
	static const uint16_t frmctrl1[] = { 0x00, 0x18 };
	static const uint16_t disctrl[] = { 0x08, 0x82, 0x27 };
	static const uint16_t enable3g[] = { 0x00 };
	static const uint16_t gammaset[] = { 0x01 };
	static const uint16_t pgamma[] = { 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08,
					   0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03,
					   0x0E, 0x09, 0x00 };
	static const uint16_t ngamma[] = { 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07,
					   0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C,
					   0x31, 0x36, 0x0F };

	lcd_cmd(0x01);		/* soft reset */
	k_msleep(120);
	lcd_cmd(0x28);		/* display off */
	lcd_cmd(0x3A);
	lcd_dat(0x55);
	lcd_write_reg(0xCF, (const uint16_t[]){ 0x00, 0xC1, 0x30 }, 3);
	lcd_write_reg(0xED, (const uint16_t[]){ 0x64, 0x03, 0x12, 0x81 }, 4);
	lcd_write_reg(0xE8, (const uint16_t[]){ 0x85, 0x00, 0x78 }, 3);
	lcd_write_reg(0xCB, (const uint16_t[]){ 0x39, 0x2C, 0x00, 0x34, 0x02 }, 5);
	lcd_write_reg(0xF7, (const uint16_t[]){ 0x20 }, 1);
	lcd_write_reg(0xEA, (const uint16_t[]){ 0x00, 0x00 }, 2);
	lcd_write_reg(0xC0, pwr1, 1);
	lcd_write_reg(0xC1, pwr2, 1);
	lcd_write_reg(0xC5, vmctrl1, 2);
	lcd_write_reg(0xC7, vmctrl2, 1);
	lcd_write_reg(0x36, madctl, 1);		/* MADCTL */
	lcd_write_reg(0x3A, pixfmt, 1);		/* COLMOD RGB565 */
	lcd_write_reg(0xB1, frmctrl1, 2);
	lcd_write_reg(0xB6, disctrl, 3);
	lcd_write_reg(0xF2, enable3g, 1);
	lcd_write_reg(0x26, gammaset, 1);
	lcd_write_reg(0xE0, pgamma, 15);
	lcd_write_reg(0xE1, ngamma, 15);
	lcd_cmd(0x11);		/* sleep out */
	k_msleep(120);
	lcd_cmd(0x29);		/* display on */
	k_msleep(20);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	lcd_cmd(0x2A);
	lcd_dat(x0 >> 8);
	lcd_dat(x0 & 0xff);
	lcd_dat(x1 >> 8);
	lcd_dat(x1 & 0xff);
	lcd_cmd(0x2B);
	lcd_dat(y0 >> 8);
	lcd_dat(y0 & 0xff);
	lcd_dat(y1 >> 8);
	lcd_dat(y1 & 0xff);
	lcd_cmd(0x2C);		/* memory write */
}

static void lcd_bars(void)
{
	static const uint16_t colors[] = {
		0xF800, 0x07E0, 0x001F, 0xFFFF,
		0xFFE0, 0x07FF, 0xF81F, 0x0000,
	};

	for (size_t c = 0; c < ARRAY_SIZE(colors); c++) {
		uint16_t x0 = (lcd_w * c) / ARRAY_SIZE(colors);
		uint16_t x1 = (lcd_w * (c + 1)) / ARRAY_SIZE(colors) - 1;

		lcd_set_window(x0, 0, x1, lcd_h - 1);
		for (uint32_t i = 0; i < (uint32_t)(x1 - x0 + 1) * lcd_h; i++) {
			lcd_dat(colors[c]);
		}
	}
}

static int cmd_lcd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* 背光 PB5 */
	const struct device *gpiob = DEVICE_DT_GET(DT_NODELABEL(gpiob));
	int ret;

	if (!device_is_ready(gpiob)) {
		shell_error(sh, "gpiob not ready");
		return -EIO;
	}
	gpio_pin_configure(gpiob, 5, GPIO_OUTPUT_ACTIVE);

	ret = lcd_probe(sh);
	if (ret == -ENODEV) {
		gpio_pin_set(gpiob, 5, 0);
		return -ENODEV;
	}

	if (lcd_w == 240) {
		shell_print(sh, "initializing ILI9341 ...");
		ili9341_init();
		lcd_bars();
		shell_print(sh, "done: %ux%u 8 色彩条 (背光 PB5 已开)",
			    lcd_w, lcd_h);
		return 0;
	}
	if (lcd_w == 480) {
		/* NT35510 初始化序列较长, 待用到 4.3 寸屏时补充 */
		shell_print(sh, "NT35510 已识别, 初始化序列待补充");
		return -ENOTSUP;
	}
	shell_error(sh, "detected controller not supported yet");
	return -ENOTSUP;
}
SHELL_CMD_REGISTER(lcd, NULL, "TFTLCD (FMC 8080) probe + color bars", cmd_lcd);
