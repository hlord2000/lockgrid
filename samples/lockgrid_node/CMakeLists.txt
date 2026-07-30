# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(ENV{ZEPHYR_EXTRA_MODULES} "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(lockgrid_node)

target_sources(app PRIVATE src/main.c)

if(CONFIG_SOC_SERIES_BSIM_NRFXX)
  # Command line argument handling on the simulated board comes from BabbleSim's
  # own utility library.
  zephyr_include_directories(
    ${BSIM_COMPONENTS_PATH}/libUtilv1/src/
    ${BSIM_COMPONENTS_PATH}/libPhyComv1/src/
  )
endif()
