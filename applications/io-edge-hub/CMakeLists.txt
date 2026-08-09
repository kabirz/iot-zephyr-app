# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(io_edge_hub)

target_include_directories(app PRIVATE include src/sys src/storage src/ftp_server)

target_sources(app PRIVATE
  src/main.c
  src/modbus/function.c
  src/modbus/init.c
  src/modbus/tcp.c
  src/modbus/rtu.c
  src/modbus/adc.c
  src/modbus/dio.c
  src/modbus/history.c
  src/udp.c
  src/can.c
  src/storage/fs_littlefs.c
  src/sys/time.c
  src/sys/watchdog.c
  src/ftp_server/ftpd.c
)

