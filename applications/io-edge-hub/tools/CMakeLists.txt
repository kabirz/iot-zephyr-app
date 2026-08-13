cmake_minimum_required(VERSION 3.10)
project(firmware_upgrade
        VERSION 1.0.0
        DESCRIPTION "io-edge-hub 固件升级 CLI (Linux C, UDP + SocketCAN)"
        LANGUAGES C)

# C 标准 (gnu11, 与原 Makefile 一致; gnu 允许 Linux 扩展如 <linux/can.h>)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # gnu11

# 警告 + 优化 (与原 Makefile 一致)
add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -O2
)

# 静态链接 glibc 可选: -DCMAKE_EXE_LINKER_FLAGS=-static
# 默认动态链接 (二进制 ~30KB).

add_executable(firmware_upgrade firmware_upgrade.c)

# 安装目标 (默认 /usr/local/bin, 可用 -DCMAKE_INSTALL_PREFIX=/usr 覆盖)
install(TARGETS firmware_upgrade
        RUNTIME DESTINATION bin)

# 烟雾测试: cmake --build . && ctest
enable_testing()
add_test(NAME smoke_help
         COMMAND firmware_upgrade --help)
set_tests_properties(smoke_help PROPERTIES
                     PASS_REGULAR_EXPRESSION "io-edge-hub 固件升级 CLI")

# 坏镜像应非 0 退出 (退出码 1 = 镜像错误)
add_test(NAME smoke_bad_image
         COMMAND firmware_upgrade upgrade
                 -f ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
                 -i 127.0.0.1)
set_tests_properties(smoke_bad_image PROPERTIES WILL_FAIL TRUE)
