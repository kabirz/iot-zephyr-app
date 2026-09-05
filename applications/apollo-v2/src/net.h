/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_NET_H
#define APOLLO_NET_H

#include <stdbool.h>
#include <stddef.h>

/* 以太网 bring-up: DHCP 优先, 超时回退静态 IP (V1 行为) */

/* 当前 IPv4 点分字符串; 无地址时返回 NULL */
const char *net_ip_str(void);

/* 地址来源 */
enum net_ip_source {
	NET_IP_NONE = 0,
	NET_IP_DHCP,
	NET_IP_STATIC_FALLBACK,
};

enum net_ip_source net_ip_source(void);

/* 地址已就绪 (DHCP 租约或静态回退已生效) */
bool net_ready(void);

/* 启动网络线程 */
void net_start(void);

#endif /* APOLLO_NET_H */
