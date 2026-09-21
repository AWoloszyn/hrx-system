// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#define SOURCE_SUM(left, right) ((left) + (right))
inline int source_sum(int left, int right) {
  /* λ */ return SOURCE_SUM(left, right);
}
