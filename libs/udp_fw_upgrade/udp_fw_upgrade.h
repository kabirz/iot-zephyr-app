/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 固件升级库 - 自包含实现 (参考 can_fw_upgrade)
 *
 * 库通过 SYS_INIT 自动创建配置端口 UDP socket (INADDR_ANY:8600),
 * 自管 RX 线程, 内部处理固件升级命令 (FW_START/DATA/END);
 * 其他配置命令通过应用注册的回调分发, 应用无需调用任何 init。
 *
 * 固件升级协议 (配置端口, 帧 [cmd 1B][data...]):
 *   FW_START 0x1 [size 4B LE][keyhash 32B opt]  → 回 [0x1][status]
 *                      [keyhash 32B] 可选: 仅在签名 key 已配置 (默认开启) 时校验
 *                      时, 且上位机携带该字段 (len==4+32) 才校验; 不一致 →
 *                      回 [0x1][2], 拒绝升级. 老上位机发 4B 帧 (不带 keyhash)
 *                      仍放行 (兼容旧协议).
 *                      status: 0=启动失败, 1=已开始, 2=keyhash 不一致
 *   FW_DATA  0x2 [data ≤511B]  → 回 [0x2][offset 4B LE]
 *   FW_END   0x3 [test 1B][crc 2B LE] → 回 [0x3][1/0]
 */

#ifndef __UDP_FW_UPGRADE_H__
#define __UDP_FW_UPGRADE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * 应用命令回调 (注入): 库 RX 线程收到非固件升级命令时调用.
 * 在库的 RX 线程上下文执行, 不可长时间阻塞.
 *
 * @param cmd       命令码 (帧首字节, 已去掉)
 * @param data      命令数据 (cmd 之后的内容)
 * @param len       命令数据长度
 * @param user_data 注册时透传的指针
 * @return true 已处理该命令; false 未处理 (库会告警 unhandled)
 *
 * 回复通过 udp_fw_reply() 发送 (库自管 socket + 回复路由).
 */
typedef bool (*udp_fw_app_cmd_cb_t)(uint8_t cmd, const uint8_t *data,
				    size_t len, void *user_data);

/**
 * @brief 添加业务命令回调 (可多次调用注册多个)
 *
 * 库 RX 线程收到非固件升级命令时, 按注册顺序广播给所有已注册的回调。
 * 建议在主线程初始化阶段调用 (库 RX 线程 SYS_INIT 后启动, 初始化时机足够早)。
 *
 * @param cb        回调 (NULL 无操作)
 * @param user_data 透传给回调
 */
void udp_fw_set_app_handler(udp_fw_app_cmd_cb_t cb, void *user_data);

/**
 * @brief 移除已注册的业务命令回调
 * @return 0 成功, -ENOENT 未找到
 */
int udp_fw_remove_handler(udp_fw_app_cmd_cb_t cb);

/**
 * @brief 通过配置端口回复上位机
 *
 * 库自管回复路由: 同子网单播到发送方, 跨子网定向广播.
 * 应用在命令回调里调用此函数回复业务命令结果.
 *
 * 跨子网 (广播) 回复默认关闭, 仅对已通过 udp_fw_allow_broadcast_cmd()
 * 放行的命令发送, 避免广播回复淹没子网内所有设备.
 *
 * @param cmd  回复命令码 (通常与请求相同)
 * @param data 回复数据 (NULL 表示无数据)
 * @param len  回复数据长度
 */
void udp_fw_reply(uint8_t cmd, const uint8_t *data, uint8_t len);

/**
 * @brief 放行某命令的跨子网广播回复
 *
 * 配置端口支持广播接收, 但广播回复会送达子网内所有设备. 默认任何命令
 * 都不允许广播回复 (跨子网接收时静默丢弃); 调用此函数注册的命令在
 * 跨子网接收时仍会以广播回复 (如网络发现命令 GET_NET/SET_NET).
 *
 * @param cmd 允许广播回复的命令码
 */
void udp_fw_allow_broadcast_cmd(uint8_t cmd);

/**
 * @brief 注册重启前钩子
 *
 * 库在执行 sys_reboot() 前按注册顺序依次调用所有已注册的钩子.
 * 适用于刷新文件系统缓存 (history) 等需要在重启前完成的操作.
 *
 * @param hook 回调函数 (NULL 无操作)
 */
typedef void (*udp_fw_pre_reboot_hook_t)(void);
void udp_fw_add_pre_reboot_hook(udp_fw_pre_reboot_hook_t hook);

#endif /* __UDP_FW_UPGRADE_H__ */
