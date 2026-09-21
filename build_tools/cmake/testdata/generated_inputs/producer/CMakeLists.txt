# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_custom_command(
  OUTPUT "${_GENERATED_INPUT}"
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/../value.txt"
    "-DOUTPUT=${_GENERATED_INPUT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/generate.cmake"
  DEPENDS ../value.txt generate.cmake
  VERBATIM
)
add_custom_target(generate_input DEPENDS "${_GENERATED_INPUT}")
iree_register_generated_compile_input(generate_input OUTPUTS "${_GENERATED_INPUT}")
