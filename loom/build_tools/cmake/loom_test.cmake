# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Source checks use the same merge, test-root link, correctness, and benchmark
# tools as the Bazel loom_test rule. RUNNER_ARGS carries shared configuration and
# case selection; ARGS contains correctness-only options such as instrumentation.
function(loom_test)
  if(NOT IREE_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(
    _RULE "" "NAME;INPUT_FORMAT;RESOURCE_GROUP" "SRCS;LIBRARIES;DATA;INPUTOPTS;ARGS;RUNNER_ARGS;LABELS;SANITIZER_SUPPRESSIONS" ${ARGN}
  )
  if(_RULE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown loom_test arguments: ${_RULE_UNPARSED_ARGUMENTS}")
  endif()
  loom_module(
    NAME "${_RULE_NAME}_library"
    SRCS ${_RULE_SRCS}
    LIBRARIES ${_RULE_LIBRARIES}
    DATA ${_RULE_DATA}
    INPUT_FORMAT "${_RULE_INPUT_FORMAT}"
    INPUTOPTS ${_RULE_INPUTOPTS}
    MODE merge
    OUTPUT_FORMAT bc
    STRICT_DEPS
  )
  loom_module(
    NAME "${_RULE_NAME}_module"
    SRCS "::${_RULE_NAME}_library"
    LIBRARIES ${_RULE_LIBRARIES}
    MODE link
    OUTPUT_FORMAT bc
    INCLUDE_INPUT_TESTS
  )
  set(_MODULE "${CMAKE_CURRENT_BINARY_DIR}/${_RULE_NAME}_module.loombc")
  iree_native_test(
    NAME "${_RULE_NAME}"
    SRC loom::tools::iree-test-loom
    ARGS "{{${_MODULE}}}" ${_RULE_RUNNER_ARGS} ${_RULE_ARGS}
    DATA ${_RULE_DATA}
    LABELS ${_RULE_LABELS}
    RESOURCE_GROUP "${_RULE_RESOURCE_GROUP}"
    SANITIZER_SUPPRESSIONS ${_RULE_SANITIZER_SUPPRESSIONS}
  )
  iree_native_test(
    NAME "${_RULE_NAME}_benchmark"
    SRC loom::tools::iree-benchmark-loom
    ARGS "{{${_MODULE}}}" ${_RULE_RUNNER_ARGS} --iterations=1 --warmup-iterations=0
      --output-format=jsonl --compile-report=none
    DATA ${_RULE_DATA}
    LABELS ${_RULE_LABELS}
    RESOURCE_GROUP "${_RULE_RESOURCE_GROUP}"
    SANITIZER_SUPPRESSIONS ${_RULE_SANITIZER_SUPPRESSIONS}
  )
endfunction()
