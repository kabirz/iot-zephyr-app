# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(apollo_v2)

target_include_directories(app PRIVATE src)

# apps 仓库根 (应用在 apps/applications/<app>/, 公共资产在 apps/fonts)
set(apps_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_sources(app PRIVATE
	src/main.c
	src/board_io.c
	src/app_time.c
	src/shell.c
)

target_sources_ifdef(CONFIG_APOLLO_STORAGE app PRIVATE src/storage.c)
target_sources_ifdef(CONFIG_APOLLO_NET app PRIVATE src/net.c)
target_sources_ifdef(CONFIG_APOLLO_CAN app PRIVATE src/can.c)
target_sources_ifdef(CONFIG_APOLLO_USB app PRIVATE src/usb.c)
target_sources_ifdef(CONFIG_APOLLO_DISPLAY app PRIVATE
	src/display.c
	${apps_root}/fonts/apollo_cjk_16.c
)
# 字体文件用 "lvgl.h" 简单包含
target_compile_definitions_ifdef(CONFIG_APOLLO_DISPLAY app PRIVATE
	LV_LVGL_H_INCLUDE_SIMPLE
)

# git 短哈希 -> fw_gitver.h (与 libs 固件升级库共用 gen_gitver.py)
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
add_custom_command(
	OUTPUT ${gen_dir}/fw_gitver.h
	COMMAND ${PYTHON_EXECUTABLE} ${apps_root}/libs/gen_gitver.py
		${gen_dir}/fw_gitver.h ${apps_root}
	DEPENDS ${apps_root}/libs/gen_gitver.py
	COMMENT "Generating fw_gitver.h"
	VERBATIM
)
add_custom_target(apollo_gitver DEPENDS ${gen_dir}/fw_gitver.h)
add_dependencies(app apollo_gitver)
