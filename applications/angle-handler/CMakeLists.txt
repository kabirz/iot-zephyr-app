# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(angle_handler)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/adc.c
  src/can.c
  src/common.c
  src/display.c
  src/font_8x16.c
  src/font_5x8.c
  src/gpio.c
  src/rf24.c
  src/main.c
  src/persist.c
)
target_sources_ifdef(CONFIG_SHELL app PRIVATE
  src/rf24_shell.c
)
