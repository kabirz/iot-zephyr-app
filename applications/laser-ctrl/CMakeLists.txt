# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(laser_ctrl)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/laser_serial.c
  src/flash.c
  src/can.c
  src/firmware.c
)

dt_nodelabel(dts_partition_path NODELABEL "spi_laser_fpga")
if (DEFINED dts_partition_path)
  target_sources(app PRIVATE src/fpga_spi.c)
endif()

