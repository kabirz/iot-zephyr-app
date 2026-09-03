/*
 * Motorcomm YT8512 / YT8512C 10/100 Ethernet PHY driver.
 *
 * 移植自 Zephyr 上游 phy_motorcomm_yt8521.c (NXP 贡献, Apache-2.0)。
 * 两颗芯片共享 MII 寄存器布局: 特定状态寄存器 0x11 (bit10 实时链路、
 * bit13 双工、bit15:14 速率)、扩展页 0x1E/0x1F。差异点:
 *
 *  - PHY ID: MII 寄存器 0x03 的 bits 9:4 为 Type_No, YT8512 = 0x12
 *    (YT8521 为 0x11 / YT8531 为 0xE9, 上游驱动只认这两个, 对 YT8512
 *    会 "timeout to get PHY ID"), 比较时忽略 bits 3:0 版本号。
 *  - 扩展寄存器布局不同: YT8521 的 0xA000 空间选择 / 0xA003 RGMII 延迟 /
 *    0xA012 SYNCE / WOL 配置在 YT8512 上是 0x200A/0x4000 系列, 全部跳过;
 *    本板 RMII 模式无需任何扩展寄存器配置。
 *  - 增加 reset-gpios 支持: 阿波罗 V2 板的 PHY 复位挂在 PCF8574
 *    IO 扩展器的 P7 脚 (低有效)。
 */

#define DT_DRV_COMPAT motorcomm_yt8512

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/mdio.h>
#include <zephyr/net/mii.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(phy_motorcomm_yt8512, CONFIG_PHY_LOG_LEVEL);

#include "phy_mii.h"

/* MII 寄存器 0x03: bits 15:10 = OUI, bits 9:4 = Type_No, bits 3:0 = 版本号 */
#define YT8512_TYPE_NO			0x12U
#define YT8512_TYPE_NO_MASK		GENMASK(9, 4)

/* PHY 特定状态寄存器 0x11 (与 YT8521 相同的位布局) */
#define YT8512_SSR_REG			0x11
#define YT8512_SSR_LINK			BIT(10)
#define YT8512_SSR_DUPLEX_FULL		BIT(13)
#define YT8512_SSR_SPEED_MASK		(0x3U << 14)
#define YT8512_SSR_SPEED_10		(0x0U << 14)
#define YT8512_SSR_SPEED_100		(0x1U << 14)

/* 自协商进行中时的轮询周期 */
#define MII_AUTONEG_POLL_INTERVAL_MS	100

#if DT_ANY_INST_HAS_PROP_STATUS_OKAY(reset_gpios)
#define ANY_RESET_GPIO 1
#endif

struct yt8512_config {
	uint8_t phy_addr;
	const struct device *mdio;
	enum phy_link_speed default_speeds;
#if ANY_RESET_GPIO
	const struct gpio_dt_spec reset_gpio;
	uint32_t reset_assert_duration_us;
	uint32_t reset_deassertion_timeout_ms;
#endif
};

struct yt8512_data {
	const struct device *dev;
	phy_callback_t cb;
	void *cb_data;
	struct phy_link_state state;
	struct k_sem sem;
	struct k_work_delayable monitor_work;
	bool autoneg_in_progress;
	k_timepoint_t autoneg_timeout;
};

static int yt8512_get_link_state(const struct device *dev, struct phy_link_state *state);

static int yt8512_read(const struct device *dev, uint16_t reg, uint32_t *data)
{
	const struct yt8512_config *config = dev->config;

	/* 清掉多余的高 16 位 */
	*data = 0U;

	return mdio_read(config->mdio, config->phy_addr, reg, (uint16_t *)data);
}

static int yt8512_write(const struct device *dev, uint16_t reg, uint32_t data)
{
	const struct yt8512_config *config = dev->config;

	return mdio_write(config->mdio, config->phy_addr, reg, (uint16_t)data);
}

static int yt8512_modify(const struct device *dev, uint16_t reg, uint16_t mask, uint16_t set)
{
	uint32_t data = 0;
	uint32_t new = 0;
	int ret;

	ret = yt8512_read(dev, reg, &data);
	if (ret) {
		return ret;
	}

	new = (data & ~mask) | set;
	if (new == data) {
		return 0;
	}

	return yt8512_write(dev, reg, new);
}

static int yt8512_soft_reset(const struct device *dev)
{
	int max_cnt = 500; /* 复位最长 ~500ms */
	uint32_t data;
	int ret;

	ret = yt8512_modify(dev, MII_BMCR, 0, MII_BMCR_RESET);
	if (ret) {
		return ret;
	}

	while (max_cnt--) {
		k_msleep(1);

		ret = yt8512_read(dev, MII_BMCR, &data);
		if (ret) {
			break;
		}

		if (!(data & MII_BMCR_RESET)) {
			ret = 0;
			break;
		}
	}

	return ret;
}

#if ANY_RESET_GPIO
static int yt8512_hard_reset(const struct device *dev)
{
	const struct yt8512_config *const cfg = dev->config;
	int ret;

	if (!gpio_is_ready_dt(&cfg->reset_gpio)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure RST pin (%d)", ret);
		return ret;
	}

	k_busy_wait(cfg->reset_assert_duration_us);

	ret = gpio_pin_set_dt(&cfg->reset_gpio, 0);
	if (ret < 0) {
		LOG_ERR("Failed to de-assert RST pin (%d)", ret);
		return ret;
	}

	k_msleep(cfg->reset_deassertion_timeout_ms);

	return 0;
}
#endif /* ANY_RESET_GPIO */

/* 清掉 BMCR 掉电位 (YT8512 无 YT8521 的 WOL 扩展寄存器检查) */
static int yt8512_resume(const struct device *dev)
{
	return yt8512_modify(dev, MII_BMCR, MII_BMCR_POWER_DOWN, 0);
}

static int yt8512_get_id(const struct device *dev, uint32_t *phy_id)
{
	const struct yt8512_config *const config = dev->config;
	uint32_t reg2 = 0, reg3 = 0;
	int ret = -EIO;

	/* MDIO 读失败时重试 (PHY 刚解除复位时可能未就绪) */
	for (uint32_t cnt = 100; cnt > 0; cnt--) {
		ret = yt8512_read(dev, MII_PHYID2R, &reg3);
		if (ret == 0) {
			break;
		}
		k_msleep(1);
	}

	if (ret) {
		LOG_ERR("PHY (%d) MDIO read failed: %d", config->phy_addr, ret);
		return ret;
	}

	if (((uint16_t)reg3 & YT8512_TYPE_NO_MASK) != (YT8512_TYPE_NO << 4)) {
		LOG_ERR("PHY (%d) reg3=0x%04X is not a YT8512 (Type_No != 0x%02X)",
			config->phy_addr, (uint16_t)reg3, YT8512_TYPE_NO);
		return -EIO;
	}

	if (yt8512_read(dev, MII_PHYID1R, &reg2) == 0) {
		uint32_t id = (reg2 << 16) | (uint16_t)reg3;

		LOG_INF("PHY (%d) ID:0x%X", config->phy_addr, id);

		if (phy_id != NULL) {
			*phy_id = id;
		}
	}

	return 0;
}

static void invoke_link_cb(const struct device *dev)
{
	struct yt8512_data *const data = dev->data;
	struct phy_link_state state;

	if (data->cb == NULL) {
		return;
	}

	yt8512_get_link_state(dev, &state);

	data->cb(data->dev, &state, data->cb_data);
}

static inline enum phy_link_speed yt8512_speed_from_ssr(uint16_t ssr)
{
	enum phy_link_speed speed;

	switch (ssr & (YT8512_SSR_SPEED_MASK | YT8512_SSR_DUPLEX_FULL)) {
	case YT8512_SSR_SPEED_10 | YT8512_SSR_DUPLEX_FULL:
		speed = LINK_FULL_10BASE;
		break;
	case YT8512_SSR_SPEED_10:
		speed = LINK_HALF_10BASE;
		break;
	case YT8512_SSR_SPEED_100 | YT8512_SSR_DUPLEX_FULL:
		speed = LINK_FULL_100BASE;
		break;
	case YT8512_SSR_SPEED_100:
		speed = LINK_HALF_100BASE;
		break;
	default:
		speed = 0;
		break;
	}

	return speed;
}

static int update_link_state(const struct device *dev)
{
	const struct yt8512_config *const cfg = dev->config;
	struct yt8512_data *const data = dev->data;
	uint32_t ssr;
	uint32_t bmcr;
	bool link_up;

	if (yt8512_read(dev, YT8512_SSR_REG, &ssr) < 0) {
		return -EIO;
	}

	link_up = (uint16_t)ssr & YT8512_SSR_LINK;

	/* 链路断开, 到此为止 */
	if (!link_up) {
		data->state.speed = 0;
		if (link_up != data->state.is_up) {
			data->state.is_up = false;
			LOG_INF("PHY (%d) is down", cfg->phy_addr);
			return 0;
		}
		return -EAGAIN;
	}

	if (yt8512_read(dev, MII_BMCR, &bmcr) < 0) {
		return -EIO;
	}

	/* 自协商未使能时, 直接读协商结果 */
	if ((bmcr & MII_BMCR_AUTONEG_ENABLE) == 0U) {
		enum phy_link_speed new_speed = yt8512_speed_from_ssr((uint16_t)ssr);

		if ((data->state.speed != new_speed) || !data->state.is_up) {
			data->state.is_up = true;
			data->state.speed = new_speed;

			LOG_INF("PHY (%d) Link speed %s Mb, %s duplex", cfg->phy_addr,
				PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100" : "10",
				PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");

			return 0;
		}
		return -EAGAIN;
	}

	/* 自协商使能且上次检查时链路已 up: 状态未变, 直接返回;
	 * 链路从 down 变 up, 则启动自协商完成检查 */
	if (data->state.is_up) {
		return -EAGAIN;
	}

	data->state.is_up = true;

	LOG_DBG("PHY (%d) Starting MII PHY auto-negotiate sequence", cfg->phy_addr);

	data->autoneg_timeout = sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	return -EINPROGRESS;
}

static int check_autonegotiation_completion(const struct device *dev)
{
	const struct yt8512_config *const cfg = dev->config;
	struct yt8512_data *const data = dev->data;

	uint32_t ssr = 0;
	uint32_t bmsr = 0;

	/* 部分 PHY 的 BMSR 位是锁存的, 第一次读可能不对, 双读取实时值 */
	if (yt8512_read(dev, MII_BMSR, &bmsr) < 0) {
		return -EIO;
	}

	if (yt8512_read(dev, MII_BMSR, &bmsr) < 0) {
		return -EIO;
	}

	if ((bmsr & MII_BMSR_AUTONEG_COMPLETE) == 0U) {
		if (sys_timepoint_expired(data->autoneg_timeout)) {
			LOG_DBG("PHY (%d) auto-negotiate timeout", cfg->phy_addr);
			return -ETIMEDOUT;
		}
		return -EINPROGRESS;
	}

	LOG_DBG("PHY (%d) auto-negotiate sequence completed", cfg->phy_addr);

	if (yt8512_read(dev, YT8512_SSR_REG, &ssr) < 0) {
		return -EIO;
	}

	data->state.speed = yt8512_speed_from_ssr((uint16_t)ssr);

	data->state.is_up = (bmsr & MII_BMSR_LINK_STATUS) != 0U;

	LOG_INF("PHY (%d) Link speed %s Mb, %s duplex", cfg->phy_addr,
		PHY_LINK_IS_SPEED_100M(data->state.speed) ? "100" : "10",
		PHY_LINK_IS_FULL_DUPLEX(data->state.speed) ? "full" : "half");

	return 0;
}

static void monitor_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct yt8512_data *const data = CONTAINER_OF(dwork, struct yt8512_data, monitor_work);
	const struct device *dev = data->dev;
	int rc;

	if (k_sem_take(&data->sem, K_NO_WAIT) == 0) {
		if (data->autoneg_in_progress) {
			rc = check_autonegotiation_completion(dev);
		} else {
			/* 自协商不在进行中, 只更新链路状态 */
			rc = update_link_state(dev);
		}

		data->autoneg_in_progress = (rc == -EINPROGRESS);

		k_sem_give(&data->sem);

		/* 链路状态变化且已注册回调, 触发回调 */
		if (rc == 0) {
			invoke_link_cb(dev);
		}
	}

	k_work_reschedule(&data->monitor_work, data->autoneg_in_progress
						       ? K_MSEC(MII_AUTONEG_POLL_INTERVAL_MS)
						       : K_MSEC(CONFIG_PHY_MONITOR_PERIOD));
}

static int yt8512_cfg_link(const struct device *dev, enum phy_link_speed adv_speeds,
			   enum phy_cfg_link_flag flags)
{
	struct yt8512_data *const data = dev->data;
	int ret = 0;

	k_sem_take(&data->sem, K_FOREVER);

	if ((flags & PHY_FLAG_AUTO_NEGOTIATION_DISABLED) != 0U) {
		ret = phy_mii_set_bmcr_reg_autoneg_disabled(dev, adv_speeds);
		if (ret >= 0) {
			data->autoneg_in_progress = false;
			k_work_reschedule(&data->monitor_work, K_NO_WAIT);
		}
	} else {
		/* YT8512 为 10/100 PHY, 无千兆能力寄存器 (MII_1KTCR) 需求 */
		ret = phy_mii_cfg_link_autoneg(dev, adv_speeds, false);
		if (ret >= 0) {
			LOG_DBG("PHY Starting MII PHY auto-negotiate sequence");
			data->autoneg_in_progress = true;
			data->autoneg_timeout =
				sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
			k_work_reschedule(&data->monitor_work,
					  K_MSEC(MII_AUTONEG_POLL_INTERVAL_MS));
		}
	}

	if (ret == -EALREADY) {
		LOG_DBG("PHY Link already configured");
	}

	k_sem_give(&data->sem);

	return ret;
}

static int yt8512_get_link_state(const struct device *dev, struct phy_link_state *state)
{
	struct yt8512_data *const data = dev->data;

	k_sem_take(&data->sem, K_FOREVER);

	memcpy(state, &data->state, sizeof(struct phy_link_state));

	if (state->speed == 0) {
		/* speed 为 0 时链路必然 down (自协商进行中) */
		state->is_up = false;
	}

	k_sem_give(&data->sem);

	return 0;
}

static int yt8512_link_cb_set(const struct device *dev, phy_callback_t cb, void *user_data)
{
	struct yt8512_data *const data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;

	/* 立即回调一次, 通知调用方当前链路状态 */
	invoke_link_cb(dev);

	return 0;
}

static int yt8512_init(const struct device *dev)
{
	struct yt8512_data *const data = dev->data;
	const struct yt8512_config *const cfg = dev->config;
	int ret;

	k_sem_init(&data->sem, 1, 1);

	data->state.is_up = false;
	data->dev = dev;
	data->cb = NULL;

#if ANY_RESET_GPIO
	if (cfg->reset_gpio.port != NULL) {
		ret = yt8512_hard_reset(dev);
		if (ret) {
			return ret;
		}
	}
#endif /* ANY_RESET_GPIO */

	if (yt8512_get_id(dev, NULL)) {
		return -EIO;
	}

	/* 软复位 */
	ret = yt8512_soft_reset(dev);
	if (ret) {
		return -EIO;
	}

	ret = yt8512_resume(dev);
	if (ret) {
		return -EIO;
	}

	k_work_init_delayable(&data->monitor_work, monitor_work_handler);

	/* 通告默认速率并启动监控 */
	ret = yt8512_cfg_link(dev, cfg->default_speeds, 0);
	if (ret == -EALREADY) {
		data->autoneg_in_progress = true;
		data->autoneg_timeout =
			sys_timepoint_calc(K_MSEC(CONFIG_PHY_AUTONEG_TIMEOUT_MS));
	}

	/* 若 cfg_link 未调度, 这里调度监控 work */
	k_work_schedule(&data->monitor_work, K_NO_WAIT);

	return 0;
}

static DEVICE_API(ethphy, yt8512_driver_api) = {
	.get_link = yt8512_get_link_state,
	.link_cb_set = yt8512_link_cb_set,
	.cfg_link = yt8512_cfg_link,
	.read = yt8512_read,
	.write = yt8512_write,
};

#define YT8512_CONFIG(n)							\
BUILD_ASSERT(PHY_INST_GENERATE_DEFAULT_SPEEDS(n) != 0,				\
	"At least one valid speed must be configured for this driver");	\
										\
static const struct yt8512_config yt8512_config_##n = {			\
	.phy_addr = DT_INST_REG_ADDR(n),					\
	.mdio = DEVICE_DT_GET(DT_INST_BUS(n)),					\
	.default_speeds = PHY_INST_GENERATE_DEFAULT_SPEEDS(n),			\
	IF_ENABLED(ANY_RESET_GPIO, (						\
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),	\
		.reset_assert_duration_us = DT_INST_PROP_OR(n,			\
			 reset_assert_duration_us, 0),				\
		.reset_deassertion_timeout_ms = DT_INST_PROP_OR(n,			\
			 reset_deassertion_timeout_ms, 0),			\
	))									\
};

#define YT8512_DATA(n)								\
static struct yt8512_data yt8512_data_##n = {					\
	.sem = Z_SEM_INITIALIZER(yt8512_data_##n.sem, 1, 1),			\
};

#define YT8512_DEVICE(n)							\
	YT8512_CONFIG(n);							\
	YT8512_DATA(n);								\
	DEVICE_DT_INST_DEFINE(n, &yt8512_init, NULL, &yt8512_data_##n,		\
			      &yt8512_config_##n, POST_KERNEL,			\
			      CONFIG_PHY_INIT_PRIORITY, &yt8512_driver_api);

DT_INST_FOREACH_STATUS_OKAY(YT8512_DEVICE)
