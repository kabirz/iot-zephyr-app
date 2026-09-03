# SPDX-License-Identifier: Apache-2.0

# ST-LINK/V2 调试器, openocd 为默认 runner
board_runner_args(openocd --cmd-post-verify "reset halt")
board_runner_args(openocd --target-handle=_CHIPNAME.cpu0)
board_runner_args(jlink "--device=STM32H743II" "--speed=4000")
board_runner_args(pyocd "--target=stm32h743iitx")

include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
