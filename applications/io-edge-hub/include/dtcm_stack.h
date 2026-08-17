/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * DTCM (CCM RAM, 0x10000000 64KB) 线程栈放置辅助
 *
 * F407 的 CCM RAM 只挂在 Cortex-M D-Bus, DMA/外设不可见, 但 CPU 栈访问
 * 完全正常。把大栈放这里可腾出等量 SRAM 给网络缓冲 (DMA 路径)。
 *
 * 约束:
 *   - 该栈线程的局部变量不可作为 DMA 缓冲 (本项目 DMA 仅 SPI, 网络数据
 *     都在 net_buf 池, 满足)
 *   - HW_STACK_PROTECTION (MPU) 不覆盖 CCM, 这些栈失去硬件溢出保护,
 *     依赖 K_ERR_STACK_CHK_FAIL 之外的手段 (只搬业务线程, 内核线程不动)
 *   - 板 DTS 需有 ccm0 节点 (zephyr,dtcm = &ccm0), 生成 .dtcm_noinit 段
 */

#ifndef __DTCM_STACK_H__
#define __DTCM_STACK_H__

#include <zephyr/kernel.h>
#include <zephyr/kernel/thread_stack.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单个栈: DTCM_STACK_DEFINE(sym, size) */
#define DTCM_STACK_DEFINE(sym, size)					\
	struct z_thread_stack_element					\
	__aligned(Z_KERNEL_STACK_OBJ_ALIGN)				\
	__section(".dtcm_noinit")					\
	sym[K_KERNEL_STACK_LEN(size)]

/* 栈数组: DTCM_STACK_ARRAY_DEFINE(sym, n, size) */
#define DTCM_STACK_ARRAY_DEFINE(sym, n, size)				\
	struct z_thread_stack_element					\
	__aligned(Z_KERNEL_STACK_OBJ_ALIGN)				\
	__section(".dtcm_noinit")					\
	sym[n][K_KERNEL_STACK_LEN(size)]

/* 大缓冲/大对象放 DTCM:
 *   DTCM_BSS(name, def)  — 零初始化或带初值均可 (dtcm_data 有 Flash 加载镜像,
 *                           dtcm_bss/noinit 零初始化, 链接器自动归类)
 *   约束: 对象不可进入 DMA 路径 (W5500 SPI2 DMA 的 net_buf 等);
 *         纯 CPU 读写 (协议解析/格式化/状态机) 才可使用。 */
#define DTCM_BSS __section(".dtcm_bss")

/* K_THREAD_DEFINE 的 DTCM 版: 线程对象在 SRAM, 栈在 DTCM.
 * 参数与 K_THREAD_DEFINE 相同。依赖内核内部宏 Z_THREAD_COMMON_DEFINE
 * (kernel.h 内部使用, 无公开替代; 升级 Zephyr 时需复核)。 */
#define K_THREAD_DTCM_DEFINE(name, stack_size, entry, p1, p2, p3,	\
			     prio, options, delay)			\
	DTCM_STACK_DEFINE(_k_thread_stack_##name, stack_size);		\
	Z_THREAD_COMMON_DEFINE(name, stack_size, entry, p1, p2, p3,	\
			       prio, options, delay)

#ifdef __cplusplus
}
#endif

#endif /* __DTCM_STACK_H__ */
