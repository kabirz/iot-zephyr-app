/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - 自包含实现
 * 库通过 SYS_INIT 自动初始化 CAN 控制器并自管 RX 线程与固件升级流程;
 * 应用只需注册一个回调处理非固件升级帧, 无需调用任何 init。
 *
 * 帧协议:
 *   0x101 平台接收 (命令)     0   START_UPDATE (size8)
 *                             1   CONFIRM (val: 0=临时 1=永久)
 *                             2   VERSION
 *                             3   REBOOT
 *   0x102 平台应答             code + offset  (见 fw_code)
 *   0x103 固件数据             [8B 数据]
 *   0x104 keyhash (默认开启)   [0]=seq(0..4), [1..7]=7B chunk, 5 帧凑齐 32B
 *   0x105 版本字符串 (设备发)  [0]=seq, [1..7]=7B 文本 (末帧 '\0' 填充)
 *   0x106 bootloader 探测 (设备发, 仅 MCUboot) [0..3]='B''T''O''1', [4..6]=M.m.p
 *   0x107 探测响应 (上位机发)  任意 1B → 设备进入固件升级等待
 *
 * MCUboot bootloader 升级 (CONFIG_CAN_FW_UPGRADE_BOOT_WAIT, boot_go_hook):
 *   MCUboot 启动时发 0x106 探测上位机 (PROBE_TIMEOUT_MS 窗口, 默认 500ms),
 *   收到 0x107 响应进入升级等待; IDLE_TIMEOUT_MS (默认 15s) 内无
 *   0x101/0x103/0x104 固件帧 (有帧则刷新) 则正常启动应用;
 *   CONFIRM 后 boot_go 在本会话内直接完成 swap (协议处理与本文件共用)。
 *
 * 响应码 fw_code:
 *   0 OFFSET / 1 UPDATE_SUCCESS / 2 VERSION / 3 CONFIRM
 *   4 FLASH_ERROR / 5 TRANSFER_ERROR / 6 KEYHASH_ERROR (keyhash 不一致, 已拒绝)
 *
 * 版本查询 (FW_CMD_VERSION):
 *   先回 0x102 code=VERSION, offset=版本字符串总长度;
 *   随后发 N 帧 0x105 分片 (每帧 1B seq + 7B 文本), 拼成 "v<M>.<m>.<p>_<6hex>".
 *
 * 升级 keyhash 校验 (默认开启, 由 CONFIG_MCUBOOT_SIGNATURE_KEY_FILE 提供 key):
 *   新上位机在发 0x101 START_UPDATE 前先发 5 帧 0x104 (32B keyhash 分帧,
 *   每帧 1B seq + 7B 数据), 全部到齐后 START_UPDATE 才校验; 不一致 → 回 KEYHASH_ERROR 且不擦 slot1。
 *   老上位机不发 key 帧 (协议无 keyhash 概念) 仍按原流程放行, 兼容旧上位机。
 *   未配置签名 key 的构建不生成 keyhash 头, 也不做校验。
 */

#ifndef __CAN_FW_UPGRADE_H__
#define __CAN_FW_UPGRADE_H__

#include <zephyr/drivers/can.h>
#include <stdbool.h>

/**
 * 应用帧回调 (注入): 库 RX 线程收到非固件升级帧时调用。
 * 在库的 RX 线程上下文执行, 不可长时间阻塞。
 * @return true 已处理该帧; false 未处理 (所有 handler 均返回 false 时,
 *              库 RX 线程会告警 "unhandled CAN frame")
 */
typedef bool (*can_fw_app_rx_cb_t)(struct can_frame *frame, void *user_data);

/**
 * @brief 添加业务帧回调 (可多次调用注册多个)
 *
 * 库 RX 线程收到非固件升级帧时, 按注册顺序广播给所有已注册的回调。
 * 建议在初始化阶段 (RX 线程活跃前) 调用, 避免与 RX 线程并发。
 *
 * @param cb        回调 (NULL 无操作)
 * @param user_data 透传给回调
 * @return CAN 设备指针; NULL 表示 handler 数组已满
 */
const struct device *can_fw_set_app_handler(can_fw_app_rx_cb_t cb, void *user_data);

/**
 * @brief 移除已注册的业务帧回调
 * @return 0 成功, -ENOENT 未找到
 */
int can_fw_remove_handler(can_fw_app_rx_cb_t cb);

/**
 * @brief 注册重启前钩子
 *
 * 库在执行 sys_reboot() 前按注册顺序依次调用所有已注册的钩子.
 * 适用于刷新文件系统缓存 (history) 等需要在重启前完成的操作.
 *
 * @param hook 回调函数 (NULL 无操作)
 */
typedef void (*can_fw_pre_reboot_hook_t)(void);
void can_fw_add_pre_reboot_hook(can_fw_pre_reboot_hook_t hook);

#endif /* __CAN_FW_UPGRADE_H__ */
