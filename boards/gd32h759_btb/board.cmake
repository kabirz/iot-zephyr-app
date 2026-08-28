# SPDX-License-Identifier: Apache-2.0

board_runner_args(pyocd "--target=gd32h759im")

board_runner_args(probe-rs "--chip=GD32H759IM")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
