# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(io_edge_hub)

target_include_directories(app PRIVATE include src/sys src/storage src/ftp_server src/web)

target_sources(app PRIVATE
  src/main.c
  src/io/dio.c
  src/io/adc.c
  src/history/history.c
  src/settings/init.c
  src/modbus/function.c
  src/modbus/tcp.c
  src/modbus/rtu.c
  src/udp.c
  src/can.c
  src/storage/fs_littlefs.c
  src/sys/time.c
  src/sys/watchdog.c
  src/ftp_server/ftpd.c
)

# 临时带宽测试端点: 仅在 CONFIG_IO_BW_TEST 启用时编译
target_sources_ifdef(CONFIG_IO_BW_TEST app PRIVATE src/bwtest.c)

# 应用调试 shell (io 命令), 依赖 SHELL (release 下自动裁剪)
target_sources_ifdef(CONFIG_IO_SHELL app PRIVATE src/shell.c)

# Web 服务 (HTTP 页面 + REST + WebSocket), CONFIG_IO_WEB 总开关
if(CONFIG_IO_WEB)
  target_sources(app PRIVATE
    src/web/httpd.c
    src/web/ws_io.c
  )

  # Web UI: index.html → gzip → C 数组 (编译进固件, HTTP 静态资源)
  set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(app
    src/web/index.html
    ${gen_dir}/web_index_html_gz.inc
    --gzip
  )

  # HTTP 资源描述 iterable section (服务 io_web 对应的链接段)
  zephyr_linker_sources(SECTIONS src/web/sections-rom.ld)
endif()
