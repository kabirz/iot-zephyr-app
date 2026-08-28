# SPDX-License-Identifier: Apache-2.0

# GD32H759 is not yet known to pyocd/probe-rs/openocd. Until a CMSIS-Pack or
# flash algorithm is wired up, flash/debug via pyocd manually (e.g. load to
# RAM through the pyocd commander) or via the GD ISP bootloader.
board_runner_args(pyocd "--target=cortexm")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
