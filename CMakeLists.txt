# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_LOCKGRID)
  zephyr_include_directories(include)
  add_subdirectory(subsys/lockgrid)
endif()
