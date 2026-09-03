/*
 * Ethernet bring-up + bandwidth test for the Alientek Apollo V2
 * STM32H743IIT6 board (YT8512C PHY, RMII).
 *
 * 静态 IPv4 192.168.12.200/24, 网线直连开发机 enp3s0 (192.168.12.100)。
 * DS0 红灯 100ms 闪烁 = 网络未就绪; DS1 绿灯 1s 闪烁 = L4 已连接。
 *
 * 测试端点 (详见 README.md):
 *   - TCP 9900: 首字节 'R' = 纯收流 (测板端 RX 带宽), 其余 = 回环 (双向)
 *   - shell: bench tx|rx|bidir (UDP 基准)
 *   - shell: mdio scan (扫描 MDIO 总线上的 PHY)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(eth_bwtest, LOG_LEVEL_INF);

static const struct gpio_dt_spec led_err =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);	/* DS0 红 */
static const struct gpio_dt_spec led_ok =
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);	/* DS1 绿 */

static struct net_mgmt_event_callback mgmt_cb;
static atomic_t net_up = ATOMIC_INIT(0);

static void event_handler(struct net_mgmt_event_callback *cb,
			  uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		atomic_set(&net_up, 1);

		char ip[NET_IPV4_ADDR_LEN];

		if (iface->config.ip.ipv4 != NULL &&
		    net_addr_ntop(AF_INET,
				  &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
				  ip, sizeof(ip)) != NULL) {
			LOG_INF("network up, IPv4 %s", ip);
		} else {
			LOG_INF("network up");
		}
	} else if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		atomic_set(&net_up, 0);
		LOG_WRN("network down");
	}
}

int main(void)
{
	int ret;

	ret = gpio_pin_configure_dt(&led_err, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("LED0 config failed: %d", ret);
	}
	ret = gpio_pin_configure_dt(&led_ok, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("LED1 config failed: %d", ret);
	}

	net_mgmt_init_event_callback(&mgmt_cb, event_handler,
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&mgmt_cb);

	LOG_INF("apollo_v2_h743iit6 eth bandwidth test");
	LOG_INF("static IPv4 " CONFIG_NET_CONFIG_MY_IPV4_ADDR
		", TCP sink/echo :9900, UDP bench 19601/19602");

	while (true) {
		if (atomic_get(&net_up)) {
			gpio_pin_set_dt(&led_err, 0);
			gpio_pin_toggle_dt(&led_ok);
			k_sleep(K_SECONDS(1));
		} else {
			gpio_pin_set_dt(&led_ok, 0);
			gpio_pin_toggle_dt(&led_err);
			k_sleep(K_MSEC(100));
		}
	}

	return 0;
}
