// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Packed framebuffer update: retain the low color bits while replacing depth.
typedef unsigned u32x16 __attribute__((vector_size(64)));

static u32x16 depth16(u32x16 previous, u32x16 depth) {
  return (previous & 65535u) | (depth & 0xffff0000u);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_depth(const u32x16* previous, const u32x16* depth, u32x16* output,
                  unsigned count) {
  for (unsigned block = 0; block < count; ++block) {
    output[block] = depth16(previous[block], depth[block]);
  }
}
