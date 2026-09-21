// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

namespace library {
struct Empty {};
struct Pair {
  unsigned first, second;
};
struct Result {
  Empty tag;
  Pair pair;
  bool valid;
};
Result update(Result value, unsigned count) {
  Result original = value;
  for (unsigned index = 0; index < count; ++index) {
    value.pair.first += value.pair.second;
    value.valid = !value.valid;
  }
  return count ? value : original;
}
}  // namespace library
