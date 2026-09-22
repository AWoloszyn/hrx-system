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

// The caller supplies 64-byte-aligned spans. Whole vectors form the body;
// the scalar tail preserves both the color bits and the exact access extent.
[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_depth_span(const unsigned* previous, unsigned* output,
                       unsigned count, unsigned depth, unsigned step) {
  const u32x16 lanes = {0u, 1u, 2u,  3u,  4u,  5u,  6u,  7u,
                        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
  const u32x16* input_vectors = reinterpret_cast<const u32x16*>(previous);
  u32x16* output_vectors = reinterpret_cast<u32x16*>(output);
  unsigned blocks = count / 16u;
  for (unsigned block = 0; block < blocks; ++block) {
    u32x16 depths = lanes * step + depth;
    output_vectors[block] = depth16(input_vectors[block], depths);
    depth += 16u * step;
  }
  for (unsigned pixel = blocks * 16u; pixel < count; ++pixel) {
    output[pixel] = (previous[pixel] & 65535u) | (depth & 0xffff0000u);
    depth += step;
  }
}
