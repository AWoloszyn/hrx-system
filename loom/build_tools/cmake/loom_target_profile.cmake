# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Typed compiler identities projected from the Bazel profile declarations.
function(loom_target_profile)
  cmake_parse_arguments(_RULE "" "NAME;FAMILY;SELECTOR" "REQUIRES" ${ARGN})
  set(_AVAILABLE TRUE)
  if(_RULE_REQUIRES)
    if(NOT (${_RULE_REQUIRES}))
      set(_AVAILABLE FALSE)
    endif()
  endif()
  iree_package_name(_PACKAGE_NAME)
  iree_package_ns(_PACKAGE_NS)
  set(_TARGET "${_PACKAGE_NAME}_${_RULE_NAME}")
  add_library("${_TARGET}" INTERFACE)
  add_library("${_PACKAGE_NS}::${_RULE_NAME}" ALIAS "${_TARGET}")
  set_target_properties("${_TARGET}" PROPERTIES
    LOOM_COMPILER_TARGET "${_RULE_FAMILY}:${_RULE_SELECTOR}"
    LOOM_PROFILE_NAME "${_RULE_NAME}"
    LOOM_PROFILE_AVAILABLE "${_AVAILABLE}")
endfunction()
