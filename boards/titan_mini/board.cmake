# SPDX-License-Identifier: Apache-2.0

# 默认烧录用 pyocd (排第一): RA8P1 固件的 hex/elf/bin 都带 OFS 选项字节记录
# (0x02C9F040), probe-rs 的 R7KA8P1KF 芯片定义没有该 flash 区, download 会报
# "No flash memory contains the entire requested memory range";
# pyocd 的 CMSIS pack 包含完整内存映射, 可正常烧录.
# probe-rs 的 attach/reset/debugserver/rtt 功能不受影响, 可正常使用.
board_runner_args(pyocd "--target=r7ka8p1kf")

board_runner_args(probe-rs "--chip=R7KA8P1KF")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
