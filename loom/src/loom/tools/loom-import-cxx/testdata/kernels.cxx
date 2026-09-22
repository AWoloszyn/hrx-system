// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "helpers.h"

[[loom::kernel, loom::workgroup_count(1, 1, 1), loom::workgroup_size(64, 1, 1)]]
void first(float* output) {
  output[0u] = scale<3>(14.0f);
}

[[loom::kernel]] void second(float* output) { output[0u] = scale<3>(7.0f); }
