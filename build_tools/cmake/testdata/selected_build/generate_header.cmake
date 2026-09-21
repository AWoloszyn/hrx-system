# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

file(READ "${INPUT}" _VALUE)
string(STRIP "${_VALUE}" _VALUE)
file(WRITE "${OUTPUT}" "#define GENERATED_VALUE ${_VALUE}\n")
