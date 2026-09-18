// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_KERNEL_H_
#define LOOMCXX_KERNEL_H_

// Launch geometry belongs on the entry with loom::workgroup_size(x, y, z) and
// loom::workgroup_count(x, y, z). Unspecified dimensions remain Loom configs.
// The corresponding *_range attributes take xmin, xmax, ymin, ymax, zmin, zmax
// and constrain those required config values with inclusive positive bounds.
// Counted unsigned for loops accept loom::unroll(factor),
// loom::pipeline(depth), and
// loom::schedule("linear"|"interleaved"|"recurrence"). Factors and depths are
// positive integer constant expressions, including template parameters. Bare
// loom::unroll requests full unrolling. Scheduling is an explicit compiler
// contract; an unsupported loop form is diagnosed instead of ignoring it.
#define LOOM_KERNEL [[loom::kernel]]
#define LOOM_DEVICE [[loom::device]]
#define LOOM_WORKGROUP [[loom::workgroup]]
#define LOOM_FORCE_INLINE [[loom::force_inline]] inline

namespace loom {

struct uint3 {
  // Coordinate along the x axis.
  unsigned x;
  // Coordinate along the y axis.
  unsigned y;
  // Coordinate along the z axis.
  unsigned z;
};

[[loom::workitem_id]] extern const uint3 workitem_id;
[[loom::workgroup_id]] extern const uint3 workgroup_id;
[[loom::workgroup_size]] extern const uint3 workgroup_size;
[[loom::workgroup_count]] extern const uint3 workgroup_count;

// Synchronizes workgroup invocations and their global/workgroup-memory
// accesses.
[[loom::barrier]] void workgroup_barrier();

// Declares a source contract on an unsigned binding and a positive upper bound.
[[loom::assume]] void assume(bool condition);

// Reads the target-selected subgroup width; no fixed wave size is implied.
[[loom::subgroup_size]] unsigned subgroup_size();

// Exchanges values across lanes selected by XOR within the given width.
[[loom::shuffle_xor]] float shuffle_xor(float value, int mask, int width);

}  // namespace loom

#endif  // LOOMCXX_KERNEL_H_
