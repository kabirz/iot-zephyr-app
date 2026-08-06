# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(n2e_gw)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/rf24.c
  src/rf24_shell.c
  src/net_shell.c
  src/config.c
  src/persist.c
  src/udp.c
  src/led.c
)
