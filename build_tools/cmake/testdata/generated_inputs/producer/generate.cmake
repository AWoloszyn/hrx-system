# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

file(READ "${INPUT}" _VALUE)
string(STRIP "${_VALUE}" _VALUE)
if(OUTPUT MATCHES "[.]c$")
  file(WRITE "${OUTPUT}" "int fixture_value(void) { return ${_VALUE}; }\n")
else()
  file(WRITE "${OUTPUT}" "#define GENERATED_VALUE ${_VALUE}\n")
endif()
