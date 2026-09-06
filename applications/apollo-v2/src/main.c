/* SPDX-License-Identifier: Apache-2.0
 * apollo-v2 主程序: 板级 IO / RTC / 存储 / 网络 / CAN 各子系统 bring-up
 * + 心跳 LED + 周期状态行。
 *
 * 实现遵循《Apollo_V2_STM32H743_Application_Plan.md》P0 阶段与 §10.2
 * "重新实现时必须保留的修复"。MCUboot/升级链路属 P1-P2, 见 README。
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#ifdef CONFIG_BOOTLOADER_MCUBOOT
#include <zephyr/dfu/mcuboot.h>	/* boot_write_img_confirmed() */
#endif

#include "version.h"
#include "board_io.h"
#include "app_time.h"
#include "storage.h"
#include "net.h"
#include "display.h"
#ifdef CONFIG_APOLLO_CAN
#include "can.h"
#endif
#ifdef CONFIG_APOLLO_USB
void apollo_usb_start(void);
#endif

LOG_MODULE_REGISTER(apollo_main, LOG_LEVEL_INF);

/* 计划 §10.3 教训 #5 的调试加强版: 打印完整 fault 现场 (PC/LR/CFSR/
 * HFSR/BFAR) 后停机不重启 —— 重启会销毁证据; 若复位时无此打印,
 * 则为硬件级复位 (BOR/NRST/看门狗), 与软件 fault 区分开。 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	printk("\n*** FATAL ERROR %u ***\n", reason);
	if (esf != NULL) {
		printk("PC =0x%08lx  LR =0x%08lx  PSR=0x%08lx\n",
		       (unsigned long)esf->basic.pc,
		       (unsigned long)esf->basic.lr,
		       (unsigned long)esf->basic.xpsr);
	}
	volatile uint32_t *cfsr = (volatile uint32_t *)0xE000ED28;
	volatile uint32_t *hfsr = (volatile uint32_t *)0xE000ED2C;
	volatile uint32_t *bfar = (volatile uint32_t *)0xE000ED38;

	printk("CFSR=0x%08lx  HFSR=0x%08lx  BFAR=0x%08lx\n",
	       (unsigned long)*cfsr, (unsigned long)*hfsr, (unsigned long)*bfar);
	printk("system HALTED (debug: 保留现场供 openocd/串口排查)\n");
	for (;;) {
		__asm__ volatile("wfi");
	}
	CODE_UNREACHABLE;
}

/* MCUboot 试运行确认: 上电自检通过并稳定运行 CONFIRM_DELAY 秒后标记
 * slot0 为 confirmed (计划 §三: 不确认 -> 下次复位回滚旧版本)。
 * shell `apollo confirm` 可立即确认; 回滚演练 = 在此之前重启。 */
#ifdef CONFIG_BOOTLOADER_MCUBOOT
#define FW_CONFIRM_DELAY_SEC 30

static bool fw_confirmed;

int apollo_fw_confirm(void)
{
	if (fw_confirmed) {
		return 0;
	}
	int ret = boot_write_img_confirmed();

	if (ret == 0) {
		fw_confirmed = true;
		LOG_INF("image confirmed: 本镜像已标记为正式版");
	} else {
		LOG_WRN("boot_write_img_confirmed: %d (可能本就非 test-pending)", ret);
	}
	return ret;
}
#else
int apollo_fw_confirm(void)
{
	return -ENOTSUP;
}
#endif

static void status_line(void)
{
	const struct apollo_storage_info *si = storage_info();
	const char *ip = net_ip_str();

	LOG_INF("up %us | %s | ip %s | qspi:%s sdram:%s sd:%s%s",
		k_uptime_get_32() / 1000,
		app_time_local_str(),
		ip ? ip : "-",
		si->qspi_ok ? "y" : "n",
		si->sdram_ok ? "y" : "n",
		si->sd_mounted ? "y" : "n",
		net_ip_source() == NET_IP_STATIC_FALLBACK ? " (static)" : "");
#ifdef CONFIG_APOLLO_CAN
	if (can_is_up()) {
		uint32_t rx, tx;

		can_get_stats(&rx, &tx);
		LOG_INF("can rx %u / tx %u", rx, tx);
	}
#endif
}

int main(void)
{
	int64_t utc;

	/* board_io_init 第一步即蜂鸣器静音 (PCF8574 跨复位保持) */
	board_io_init();

	LOG_INF("========================================");
	LOG_INF("apollo-v2 fw %s", APOLLO_FW_VERSION);
	LOG_INF("board apollo_v2_h743iit6, built " __DATE__ " " __TIME__);
	LOG_INF("========================================");

	if (app_time_get_utc(&utc)) {
		LOG_INF("RTC set, local (UTC+8): %s", app_time_local_str());
	} else {
		LOG_WRN("RTC 未设置 (shell: apollo timeset YYYY-MM-DD HH:MM:SS)");
	}

	storage_start();
	net_start();
#ifdef CONFIG_APOLLO_CAN
	apollo_can_start();
#endif
#ifdef CONFIG_APOLLO_USB
	apollo_usb_start();
#endif
#ifdef CONFIG_APOLLO_DISPLAY
	apollo_display_start();
#endif

	LOG_INF("init done");

	uint32_t half_sec = 0;
	uint32_t status_half = 0;

	if (CONFIG_APOLLO_STATUS_PERIOD > 0) {
		status_half = (uint32_t)CONFIG_APOLLO_STATUS_PERIOD * 2;
	}

	for (;;) {
		k_msleep(500);
		half_sec++;
		board_io_led_toggle(0);	/* DS0 红心跳 */

		if (status_half > 0 && (half_sec % status_half) == 0) {
			status_line();
		}
#ifdef CONFIG_BOOTLOADER_MCUBOOT
		if ((half_sec % (FW_CONFIRM_DELAY_SEC * 2)) == 0) {
			apollo_fw_confirm();
		}
#endif
	}

	return 0;
}
