/* SPDX-License-Identifier: Apache-2.0
 * 存储: QSPI W25Q256 / FMC SDRAM 开机探测 + TF 卡 FAT 挂载 (周期重试)。
 *
 * 教训 (计划 §10.3 #13): 上电瞬间 TF 卡可能未就绪, 同步挂载会失败,
 * 必须在后台线程周期重试。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <ff.h>		/* FATFS: fs_mount_t.fs_data 指向的挂载工作区 */
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include "storage.h"

LOG_MODULE_REGISTER(apollo_storage, LOG_LEVEL_INF);

#define QSPI_NODE	DT_NODELABEL(qspi_nor)
#define SDRAM_NODE	DT_NODELABEL(sdram_mem)

#define SD_MOUNT_POINT	"/SD:"
#define SD_RETRY_DELAY	K_SECONDS(2)
#define SD_RETRY_MAX	15	/* 共 ~30s, 覆盖慢卡上电 */

static struct apollo_storage_info info;

/* 注意: fs_data 必须指向 FATFS 工作区, 传 NULL 时 f_mount 变成卸载语义
 * (返回 FR_INVALID_DRIVE -> -19), 曾在这里踩坑 */
static FATFS fatfs_fs;
static struct fs_mount_t sd_mp = {
	.type = FS_FATFS,
	.mnt_point = SD_MOUNT_POINT,
	.fs_data = &fatfs_fs,
};

/* QSPI 资源区 LittleFS (Web 静态资源/字库暂存), 首挂失败自动 mkfs */
static struct fs_littlefs qspi_lfs;
static struct fs_mount_t qspi_mp = {
	.type = FS_LITTLEFS,
	.mnt_point = "/QSPI:",
	.fs_data = &qspi_lfs,
	.storage_dev = (void *)PARTITION_ID(resources_partition),
};

struct fs_mount_t *apollo_qspi_mountp(void)
{
	return &qspi_mp;
}

static void mount_qspi_lfs(void)
{
	int rc = fs_mount(&qspi_mp);

	if (rc != 0) {
		LOG_INF("QSPI LittleFS 首挂失败 (%d), mkfs ...", rc);
		rc = fs_mkfs(FS_LITTLEFS,
			     (uintptr_t)qspi_mp.storage_dev, NULL, 0);
		if (rc == 0) {
			rc = fs_mount(&qspi_mp);
		}
	}
	if (rc != 0) {
		LOG_ERR("QSPI LittleFS 挂载失败 (%d)", rc);
		return;
	}
	info.qspi_fs_mounted = true;
	LOG_INF("QSPI LittleFS: 挂载于 /QSPI: (%uKB)",
		PARTITION_SIZE(resources_partition) / 1024);
}

/* ---------------- QSPI W25Q256: 探测容量 ---------------- */
static void probe_qspi(void)
{
	const struct device *dev = DEVICE_DT_GET(QSPI_NODE);

	if (!device_is_ready(dev)) {
		LOG_WRN("QSPI W25Q256: not ready");
		return;
	}
	const size_t size = DT_PROP(QSPI_NODE, size) / 8;	/* bit -> B */

	info.qspi_ok = true;
	LOG_INF("QSPI W25Q256: %uMB ready (indirect 模式, 只探测不写)",
		(unsigned)(size >> 20));
}

/* ---------------- FMC SDRAM: 4 字走样 ---------------- */
static void probe_sdram(void)
{
	volatile uint32_t *base =
		(volatile uint32_t *)DT_REG_ADDR(SDRAM_NODE);
	const uint32_t magic = 0xA5C35017U;
	uint32_t errs = 0;

	for (size_t i = 0; i < 4; i++) {
		base[i] = magic ^ (uint32_t)i;
	}
	for (size_t i = 0; i < 4; i++) {
		if (base[i] != (magic ^ (uint32_t)i)) {
			errs++;
		}
	}

	info.sdram_ok = errs == 0;
	LOG_INF("FMC SDRAM %uMB @0x%08x: %s",
		(unsigned)(DT_REG_SIZE(SDRAM_NODE) >> 20),
		(unsigned)DT_REG_ADDR(SDRAM_NODE),
		info.sdram_ok ? "走样 OK" : "走样失败");
}

/* ---------------- TF 卡: 带重试的挂载 ---------------- */
static void mount_sd(void)
{
	static uint8_t buf[512];
	uint32_t sector_count = 0, sector_size = 0;
	int ret;

	for (int attempt = 1; attempt <= SD_RETRY_MAX; attempt++) {
		ret = disk_access_init("SD");
		if (ret == 0) {
			break;
		}
		if (attempt == SD_RETRY_MAX) {
			LOG_WRN("TF card: 未检测到卡 (%d 次尝试), 放弃", attempt);
			return;
		}
		if (attempt == 1) {
			LOG_INF("TF card: 未就绪, 后台重试中...");
		}
		k_sleep(SD_RETRY_DELAY);
	}

	if (disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT,
			      &sector_count) == 0 &&
	    disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE,
			      &sector_size) == 0 &&
	    disk_access_read("SD", buf, 0, 1) == 0) {
		LOG_INF("TF card: %uMB, MBR 签名 %s",
			(unsigned)(((uint64_t)sector_count * sector_size) >> 20),
			(buf[510] == 0x55 && buf[511] == 0xAA) ? "OK" : "无");
	}

	/* 直接调 FatFs 原生 f_mount, 拿到具体 FRESULT 便于诊断;
	 * 成功后再过 Zephyr fs 层注册 (fs_mount) */
	FRESULT res = f_mount(&fatfs_fs, "SD:", 1);

	if (res != FR_OK && res != FR_INVALID_DRIVE) {
		LOG_ERR("f_mount(\"SD:\") FRESULT=%d", res);
	}

	ret = fs_mount(&sd_mp);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("TF card: 挂载失败 (%d)", ret);
		return;
	}
	info.sd_mounted = true;
	LOG_INF("TF card: 挂载于 %s (f_mount=%d)", SD_MOUNT_POINT, res);
}

const struct apollo_storage_info *storage_info(void)
{
	return &info;
}

static void storage_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	probe_qspi();
	probe_sdram();
	mount_sd();
	mount_qspi_lfs();
}

static struct k_thread storage_thread;
static K_THREAD_STACK_DEFINE(storage_stack, CONFIG_APOLLO_STORAGE_STACK_SIZE);

void storage_start(void)
{
	k_thread_create(&storage_thread, storage_stack,
			K_THREAD_STACK_SIZEOF(storage_stack),
			storage_thread_fn, NULL, NULL, NULL,
			CONFIG_APOLLO_STORAGE_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&storage_thread, "apollo_storage");
}
