# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.26)
if(POLICY CMP0158)
  cmake_policy(SET CMP0158 NEW)
endif()
if(IREE_TEST_CROSS_COMPILE)
  # Use an explicit target system with the host ABI so execution can exercise
  # CMake's cross-compiling command rules without requiring a foreign emulator.
  set(CMAKE_SYSTEM_NAME "${CMAKE_HOST_SYSTEM_NAME}")
endif()
project(iree_cmake_test_commands C)
if(IREE_TEST_CROSS_COMPILE)
  set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
enable_testing()

set(IREE_BUILD_TESTS ON)
set(IREE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IREE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(IREE_TEST_TMPDIR_ROOT "${CMAKE_CURRENT_BINARY_DIR}/tmp")
set(IREE_PACKAGE_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IREE_PACKAGE_ROOT_PREFIX fixture)
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_macros.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_install_support.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_configure_testing.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_cc_test.cmake")
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_native_test.cmake")

set(_EMULATOR_VALUE absent)
set(_LAUNCHER_VALUE absent)
if(IREE_TEST_CROSS_COMPILE)
  set(_EMULATOR_VALUE "emulator with spaces")
  set(CMAKE_CROSSCOMPILING_EMULATOR
    "${CMAKE_COMMAND};-E;env;IREE_TEST_COMMAND_EMULATOR=${_EMULATOR_VALUE}")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
    set(_LAUNCHER_VALUE "launcher with spaces")
    set(CMAKE_TEST_LAUNCHER
      "${CMAKE_COMMAND};-E;env;IREE_TEST_COMMAND_LAUNCHER=${_LAUNCHER_VALUE}")
  endif()
endif()

set(_ARGS "argument with spaces" "literal=[value]"
  "${_EMULATOR_VALUE}" "${_LAUNCHER_VALUE}")
set(_ENV "IREE_TEST_COMMAND_ENV=authored environment")

# The native declaration precedes its executable alias. Both rules must retain
# the target's eventual location and execution properties until generation.
iree_native_test(NAME native SRC ::compiled ARGS ${_ARGS} ENV "${_ENV}")
iree_cc_test(NAME compiled SRCS command.c ARGS ${_ARGS} ENV "${_ENV}")
set_target_properties(fixture_compiled PROPERTIES OUTPUT_NAME "command with spaces")
iree_finalize_target_dependencies()
