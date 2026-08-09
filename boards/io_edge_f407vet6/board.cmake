# SPDX-License-Identifier: Apache-2.0

board_runner_args(pyocd "--target=stm32f407ve")

board_runner_args(probe-rs "--chip=STM32F407VETx")

board_runner_args(jlink "--device=STM32F407VE" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/probe-rs.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
