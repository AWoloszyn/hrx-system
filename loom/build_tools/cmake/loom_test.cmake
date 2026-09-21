# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# A source owner imports and links one module. Independently gated execution and
# compiler children consume that module without repeating its source closure.
function(loom_test)
  if(NOT IREE_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(
    _RULE "" "NAME;INPUT_FORMAT" "SRCS;LIBRARIES;DATA;INPUTOPTS" ${ARGN}
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
  iree_package_target_name(_MODULE_TARGET "::${_RULE_NAME}_module")
  # Runtime fixtures belong to the source owner, not individual environments.
  set_property(TARGET "${_MODULE_TARGET}" PROPERTY LOOM_TEST_DATA "${_RULE_DATA}")
endfunction()

# RUNNER_ARGS reaches correctness and benchmark smoke; ARGS is correctness-only.
function(loom_execution_test)
  if(NOT IREE_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(
    _RULE "" "NAME;MODULE;RESOURCE_GROUP" "ARGS;RUNNER_ARGS;LABELS;SANITIZER_SUPPRESSIONS" ${ARGN}
  )
  if(_RULE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown loom_execution_test arguments: ${_RULE_UNPARSED_ARGUMENTS}")
  endif()
  iree_package_target_name(_MODULE_TARGET "${_RULE_MODULE}")
  get_target_property(_MODULE "${_MODULE_TARGET}" LOOM_MODULE_FILE)
  get_target_property(_DATA "${_MODULE_TARGET}" LOOM_TEST_DATA)
  iree_native_test(
    NAME "${_RULE_NAME}"
    SRC loom::tools::iree-test-loom
    ARGS "{{${_MODULE}}}" ${_RULE_RUNNER_ARGS} ${_RULE_ARGS}
    DATA ${_DATA}
    LABELS ${_RULE_LABELS}
    RESOURCE_GROUP "${_RULE_RESOURCE_GROUP}"
    SANITIZER_SUPPRESSIONS ${_RULE_SANITIZER_SUPPRESSIONS}
  )
  iree_native_test(
    NAME "${_RULE_NAME}_benchmark"
    SRC loom::tools::iree-benchmark-loom
    ARGS "{{${_MODULE}}}" ${_RULE_RUNNER_ARGS} --iterations=1 --warmup-iterations=0
      --output-format=jsonl --compile-report=none
    DATA ${_DATA}
    LABELS ${_RULE_LABELS}
    RESOURCE_GROUP "${_RULE_RESOURCE_GROUP}"
    SANITIZER_SUPPRESSIONS ${_RULE_SANITIZER_SUPPRESSIONS}
  )
endfunction()
