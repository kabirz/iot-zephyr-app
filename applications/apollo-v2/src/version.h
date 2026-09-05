/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_VERSION_H
#define APOLLO_VERSION_H

/* 与应用根目录 VERSION 文件保持一致 (sysbuild/MCUboot 镜像版本用它) */
#define APOLLO_VERSION_MAJOR 0
#define APOLLO_VERSION_MINOR 5
#define APOLLO_VERSION_PATCH 0
#define APOLLO_VERSION_STRING "0.5.0"

/* fw_gitver.h 由 gen_gitver.py 在构建期生成 (无 git 时为 "000000") */
#include <fw_gitver.h>

#define APOLLO_FW_VERSION APOLLO_VERSION_STRING "-" FW_GIT_VERSION

#endif /* APOLLO_VERSION_H */
