// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern "C" int fixture_value(void);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) {
  printf("value=%d\n", fixture_value());
  return 0;
}
