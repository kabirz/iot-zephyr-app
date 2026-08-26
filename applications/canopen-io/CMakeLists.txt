# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(canopen_io)

target_include_directories(app PRIVATE include src)

target_sources(app PRIVATE
  src/main.c
  src/app_od.c
  src/fw_download.c
  src/io/dio.c
  src/io/adc.c
  objdict/OD.c
)

zephyr_include_directories(objdict)
