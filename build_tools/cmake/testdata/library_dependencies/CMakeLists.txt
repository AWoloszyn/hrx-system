# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.26)
project(iree_cmake_library_dependencies_test C)

set(IREE_CXX_STANDARD 17)
set(IREE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IREE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(IREE_PACKAGE_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IREE_PACKAGE_ROOT_PREFIX "iree/fixture")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_macros.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_install_support.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_cc_library.cmake")

iree_cc_library(NAME headers HDRS generated.h)
iree_cc_library(NAME leaf SRCS leaf.c DEFINES TRANSITIVE_VALUE=3)
iree_cc_library(NAME library SRCS library.c DEPS ::leaf ::headers)
add_executable(probe main.c)
target_link_libraries(probe PRIVATE iree::fixture::library)
iree_cc_unified_library(NAME unified ROOT ::library)
add_executable(unified_probe main.c)
target_link_libraries(unified_probe PRIVATE iree::fixture::unified)

# Consumers may precede a generated header's declaration in the package graph.
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.h"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/value.txt"
    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated.h"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_header.cmake"
  DEPENDS value.txt generate_header.cmake
  VERBATIM
)
add_custom_target(generate_header
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated.h")
iree_register_generated_compile_input(generate_header OUTPUTS generated.h)
iree_finalize_target_dependencies()

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/paths-$<CONFIG>.txt"
  CONTENT "$<TARGET_FILE:iree_fixture_leaf>\n$<TARGET_FILE:probe>\n$<TARGET_FILE:unified_probe>\n")
