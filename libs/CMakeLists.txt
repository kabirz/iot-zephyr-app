# 生成 fw_gitver.h (6位 git commit hash), 供 CAN/UDP 固件升级库共用版本字符串
# "v<M>.<m>.<p>_<6hex>". 无条件生成 (不依赖签名 key).
execute_process(
	COMMAND ${PYTHON_EXECUTABLE}
	        "${CMAKE_CURRENT_SOURCE_DIR}/gen_gitver.py"
	        "${CMAKE_CURRENT_BINARY_DIR}/fw_gitver.h"
	        "${CMAKE_CURRENT_SOURCE_DIR}")
zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})

# 统一在 libs 顶层生成各固件升级库的 keyhash 头文件.
# gen_keyhash.py 与本 CMakeLists.txt 同级, 供 CAN/UDP 升级库共用.
# key 取自 app 配置的 CONFIG_MCUBOOT_SIGNATURE_KEY_FILE; 未配置签名
# key 的构建不生成任何 keyhash 头 (不校验).
if(CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
   AND NOT "${CONFIG_MCUBOOT_SIGNATURE_KEY_FILE}" STREQUAL "")

	# 任意固件升级库启用时, 生成共享的 fw_keyhash.h (只生成一份,
	# CAN/UDP 升级库共用同一个 keyhash). 未配置签名 key 则不做校验.
	if(CONFIG_CAN_FW_UPGRADE OR CONFIG_UDP_FW_UPGRADE)
		execute_process(
			COMMAND ${PYTHON_EXECUTABLE}
			        "${CMAKE_CURRENT_SOURCE_DIR}/gen_keyhash.py"
			        "${CONFIG_MCUBOOT_SIGNATURE_KEY_FILE}"
			        "${CMAKE_CURRENT_BINARY_DIR}/fw_keyhash.h"
			        fw_keyhash)
		zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})
	endif()
endif()

# CAN/UDP 固件升级库源码 (各自由 CONFIG_<X>_FW_UPGRADE 控制).
zephyr_sources_ifdef(CONFIG_CAN_FW_UPGRADE can_fw_upgrade/can_fw_upgrade.c)
zephyr_include_directories_ifdef(CONFIG_CAN_FW_UPGRADE can_fw_upgrade)
zephyr_sources_ifdef(CONFIG_UDP_FW_UPGRADE udp_fw_upgrade/udp_fw_upgrade.c)
zephyr_include_directories_ifdef(CONFIG_UDP_FW_UPGRADE udp_fw_upgrade)