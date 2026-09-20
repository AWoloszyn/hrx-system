// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef SCHEDULE
#define SCHEDULE unroll(factor)
#endif

unsigned scheduled(unsigned count, unsigned factor) {
  unsigned total = 0;
  [[loom::SCHEDULE]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
  }
  return total;
}
