# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(_KIND h c)
  set(_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.${_KIND}")
  add_custom_command(OUTPUT "${_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/../value.txt"
      "-DOUTPUT=${_OUTPUT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/generate.cmake"
    DEPENDS ../value.txt generate.cmake VERBATIM)
  add_custom_target(generate_${_KIND} DEPENDS "${_OUTPUT}")
  iree_register_generated_compile_input(generate_${_KIND} OUTPUTS "${_OUTPUT}")
endforeach()
