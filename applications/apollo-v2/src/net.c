/* SPDX-License-Identifier: Apache-2.0
 * 以太网 bring-up (YT8512C RMII): DHCP 优先, 超时后配置静态回退地址
 * (网线直连开发机场景, V1 实测行为)。
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "net.h"

LOG_MODULE_REGISTER(apollo_net, LOG_LEVEL_INF);

static char ip_buf[INET_ADDRSTRLEN];
static enum net_ip_source ip_source = NET_IP_NONE;
/* fallback 自己 add 地址也会触发 ADDR_ADD 事件, 用该标志区分来源 */
static bool dhcp_active;

static struct net_mgmt_event_callback addr_cb;
static struct k_sem lease_sem;

static bool is_useful_addr(const struct in_addr *addr)
{
	/* 跳过链路本地 169.254.x.x */
	return sys_get_be16(addr->s4_addr) != 0xA9FE;
}

static void addr_cb_handler(struct net_mgmt_event_callback *cb,
			    uint64_t mgmt_event, struct net_if *iface)
{
	/* v4.4: 事件 info 直接是 struct in_addr */
	const struct in_addr *addr = (const struct in_addr *)cb->info;

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD || iface != net_if_get_default()) {
		return;
	}
	if (!dhcp_active || !is_useful_addr(addr)) {
		return;
	}

	net_addr_ntop(AF_INET, addr, ip_buf, sizeof(ip_buf));
	ip_source = NET_IP_DHCP;
	LOG_INF("DHCP lease: %s", ip_buf);
	k_sem_give(&lease_sem);
}

const char *net_ip_str(void)
{
	return ip_source == NET_IP_NONE ? NULL : ip_buf;
}

enum net_ip_source net_ip_source(void)
{
	return ip_source;
}

bool net_ready(void)
{
	return ip_source != NET_IP_NONE;
}

/* 静态回退: 直连开发机场景 (192.168.12.1 网关不配置, 二层直通) */
static void apply_static_fallback(struct net_if *iface)
{
	struct in_addr addr, netmask;

	dhcp_active = false;
	if (net_addr_pton(AF_INET, CONFIG_APOLLO_NET_FALLBACK_ADDR, &addr) != 0) {
		LOG_ERR("回退地址非法: %s", CONFIG_APOLLO_NET_FALLBACK_ADDR);
		return;
	}
	if (net_addr_pton(AF_INET, "255.255.255.0", &netmask) != 0) {
		return;
	}
	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("静态回退地址添加失败");
		return;
	}
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);

	strcpy(ip_buf, CONFIG_APOLLO_NET_FALLBACK_ADDR);
	ip_source = NET_IP_STATIC_FALLBACK;
	LOG_WRN("DHCP %ds 未获租约, 回退静态 %s (可直连 PC 同网段使用)",
		CONFIG_APOLLO_NET_DHCP_TIMEOUT, ip_buf);
}

static void net_thread_fn(void *p1, void *p2, void *p3)
{
	struct net_if *iface = net_if_get_default();
	int wait_ms = CONFIG_APOLLO_NET_DHCP_TIMEOUT * MSEC_PER_SEC;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (iface == NULL) {
		LOG_ERR("无网络接口");
		return;
	}

	/* 等接口 administratively up (eth 驱动 init 后自动 up) */
	for (int i = 0; i < 100 && !net_if_is_up(iface); i++) {
		k_msleep(50);
	}
	if (!net_if_is_up(iface)) {
		LOG_WRN("接口未 up, 放弃 DHCP");
		apply_static_fallback(iface);
		return;
	}

	k_sem_init(&lease_sem, 0, 1);
	net_mgmt_init_event_callback(&addr_cb, addr_cb_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&addr_cb);

	LOG_INF("eth interface up, DHCP 请求中...");
	/* DHCP 后台请求中 (拿到租约或超时前保持运行) */
	dhcp_active = true;
	net_dhcpv4_start(iface);

#ifdef CONFIG_APOLLO_WEB
	extern int apollo_web_start(void);
#endif
	if (k_sem_take(&lease_sem, K_MSEC(wait_ms)) != 0) {
		/* 必须显式停掉 DHCP 客户端: 放任其在静态地址上继续
		 * 广播 Discover 与静态配置共存, 行为未定义 */
		net_dhcpv4_stop(iface);
		apply_static_fallback(iface);
	} else {
		LOG_INF("net ready (ping 本地址可测链路)");
	}
#ifdef CONFIG_APOLLO_WEB
	apollo_web_start();
#endif
}

static struct k_thread net_thread;
static K_THREAD_STACK_DEFINE(net_stack, CONFIG_APOLLO_NET_STACK_SIZE);

void net_start(void)
{
	k_thread_create(&net_thread, net_stack,
			K_THREAD_STACK_SIZEOF(net_stack),
			net_thread_fn, NULL, NULL, NULL,
			CONFIG_APOLLO_NET_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&net_thread, "apollo_net");
}
