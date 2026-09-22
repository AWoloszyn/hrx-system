# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.21)
project(iree_cmake_data_dependencies_test C)

if(NOT IREE_REPO_ROOT)
  message(FATAL_ERROR "IREE_REPO_ROOT is required")
endif()

set(IREE_BINARY_DIR "${CMAKE_BINARY_DIR}")
set(IREE_IDE_FOLDER IREE)
include("${IREE_REPO_ROOT}/build_tools/cmake/iree_macros.cmake")

# Target identities may have spellings that CMake treats as false.
add_custom_target(data_consumer-NOTFOUND)
set(IREE_TEST_GENERATED_DATA "${CMAKE_BINARY_DIR}/generated.txt"
  CACHE FILEPATH "Generated DATA output")
iree_add_data_dependencies(
  NAME
    data_consumer-NOTFOUND
  DATA
    fixture.txt
    "${IREE_TEST_GENERATED_DATA}"
    fixture::tool
)

option(IREE_TEST_DECLARE_TOOL "Declare the later DATA tool target" ON)
set(IREE_TEST_TOOL_REFERENCE fixture::tool CACHE STRING "Later tool target spelling")
add_custom_target(tool_path_consumer)
iree_add_data_dependencies(
  NAME
    tool_path_consumer
  DATA
    "$<TARGET_FILE:${IREE_TEST_TOOL_REFERENCE}>"
)
if(IREE_TEST_DECLARE_TOOL)
  add_executable(data_tool-NOTFOUND tool.c)
  add_executable(fixture::tool ALIAS data_tool-NOTFOUND)
  iree_package_target_name(_TOOL_TARGET "${IREE_TEST_TOOL_REFERENCE}")
  set_target_properties("${_TOOL_TARGET}" PROPERTIES OUTPUT_NAME data_tool)
  add_custom_command(
    TARGET "${_TOOL_TARGET}"
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E touch
      "${CMAKE_BINARY_DIR}/tool-built.marker"
  )
endif()

add_custom_command(
  OUTPUT
    "${IREE_TEST_GENERATED_DATA}"
  COMMAND
    "${CMAKE_COMMAND}" -E copy
    "${CMAKE_CURRENT_SOURCE_DIR}/fixture.txt"
    "${IREE_TEST_GENERATED_DATA}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/fixture.txt"
)
# Producer names are identities even when CMake would treat them as false.
add_custom_target(generated_data-NOTFOUND DEPENDS "${IREE_TEST_GENERATED_DATA}")
iree_register_generated_output_producer(generated_data-NOTFOUND
  OUTPUTS "${IREE_TEST_GENERATED_DATA}"
)

add_custom_target(later_data_consumer)
iree_add_data_dependencies(
  NAME later_data_consumer
  DATA fixture.txt "${IREE_TEST_GENERATED_DATA}" fixture::tool
)

option(IREE_TEST_DUPLICATE_PRODUCER "Declare conflicting output ownership" OFF)
if(IREE_TEST_DUPLICATE_PRODUCER)
  add_custom_target(duplicate_data)
  iree_register_generated_output_producer(duplicate_data
    OUTPUTS "${IREE_TEST_GENERATED_DATA}")
endif()

iree_finalize_target_dependencies()
