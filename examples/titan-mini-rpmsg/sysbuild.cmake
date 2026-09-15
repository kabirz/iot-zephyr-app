# SPDX-License-Identifier: Apache-2.0

if("${SB_CONFIG_REMOTE_BOARD}" STREQUAL "")
  message(FATAL_ERROR "No remote board selected for ${BOARD}")
endif()

ExternalZephyrProject_Add(
  APPLICATION remote
  SOURCE_DIR  ${APP_DIR}/remote
  BOARD       ${SB_CONFIG_REMOTE_BOARD}
)
