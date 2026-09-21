# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.26)
project(iree_cmake_generated_inputs_test C CXX)

set(IREE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IREE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(IREE_PACKAGE_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IREE_PACKAGE_ROOT_PREFIX fixture)
set(IREE_CXX_STANDARD 17)
set(IREE_BUILD_TESTS ON)
set(IREE_BUILD_BENCHMARKS ON)
set(IREE_ENABLE_FUZZING ON)
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)
foreach(_MODULE macros install_support configure_testing native_test
    cc_library cc_binary cc_test cc_binary_benchmark cc_fuzz)
  include("${IREE_REPO_ROOT}/build_tools/cmake/iree_${_MODULE}.cmake")
endforeach()

if(IREE_TEST_RULE STREQUAL "fuzz")
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer)
  check_cxx_source_compiles("
    #include <stddef.h>
    #include <stdint.h>
    extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
    " IREE_TEST_HAS_LIBFUZZER)
  if(NOT IREE_TEST_HAS_LIBFUZZER)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libfuzzer-unavailable.txt" "")
    return()
  endif()
endif()

if(IREE_TEST_INPUT STREQUAL "source")
  set(_GENERATED_INPUT "${CMAKE_CURRENT_BINARY_DIR}/producer/generated.c")
  set(_SOURCES "${_GENERATED_INPUT}")
  set(_HEADERS)
else()
  set(_GENERATED_INPUT "${CMAKE_CURRENT_BINARY_DIR}/producer/generated.h")
  set(_SOURCES consumer.c)
  set(_HEADERS "${_GENERATED_INPUT}")
endif()

if(IREE_TEST_RULE MATCHES "^(library|alwayslink|interface)$")
  if(IREE_TEST_RULE STREQUAL "interface")
    if(IREE_TEST_INPUT STREQUAL "textual")
      iree_cc_library(NAME subject TEXTUAL_HDRS ${_HEADERS})
    else()
      iree_cc_library(NAME subject HDRS ${_HEADERS})
    endif()
    add_executable(probe main.c consumer.c)
  else()
    set(_MODE)
    if(IREE_TEST_RULE STREQUAL "alwayslink")
      set(_MODE ALWAYSLINK)
    endif()
    iree_cc_library(NAME subject SRCS ${_SOURCES} HDRS ${_HEADERS} ${_MODE})
    add_executable(probe main.c)
  endif()
  target_link_libraries(probe PRIVATE fixture::subject)
  set(_PROGRAM probe)
else()
  if(IREE_TEST_RULE STREQUAL "fuzz")
    iree_cc_fuzz(NAME probe SRCS fuzz.cc ${_SOURCES} ${_HEADERS})
  else()
    set(_RULE "iree_cc_${IREE_TEST_RULE}")
    if(IREE_TEST_RULE STREQUAL "benchmark")
      set(_RULE iree_cc_binary_benchmark)
    endif()
    cmake_language(CALL ${_RULE} NAME probe SRCS main.c ${_SOURCES} ${_HEADERS})
  endif()
  set(_PROGRAM fixture_probe)
endif()

# A different directory owns the custom command, and it is visited after every
# consumer. Header and source cases are separate so neither can supply a missing
# dependency for the other.
add_subdirectory(producer)
iree_finalize_target_dependencies()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/paths-$<CONFIG>.txt"
  CONTENT "${_PROGRAM}\n$<TARGET_FILE:${_PROGRAM}>\n${_GENERATED_INPUT}\n")
