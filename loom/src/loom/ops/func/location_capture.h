// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_OPS_FUNC_LOCATION_CAPTURE_H_
#define LOOM_OPS_FUNC_LOCATION_CAPTURE_H_

#include "loom/error/source.h"
#include "loom/ir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Freezes |location| and its reachable provenance into semantic nodes for
// func.location. The root is last in the returned postorder array; repeated
// children retain their identity. Coordinates and field spans are preserved,
// with one-based Unicode code-point columns and exclusive ends. Available
// original source lines are copied; absent or rendered source is never
// substituted for the original spelling.
//
// Call while the original location graph, field spans, and source snapshots are
// available, before serialization or transformations discard them. |module|
// owns the returned array and every payload. Neither |resolver| nor
// |scratch_arena| is retained. Build a func.location with these nodes to
// preserve the value through cloning, serialization, and debug stripping.
//
// The graph is trusted compiler-owned IR. Failure is limited to allocation and
// semantic attribute size bounds. Scratch storage scales with reachable nodes,
// independently of the containing module's size or the graph's nesting depth.
iree_status_t loom_func_location_capture(
    loom_module_t* module, loom_location_id_t location,
    loom_source_resolver_t resolver, iree_arena_allocator_t* scratch_arena,
    loom_parameterized_attr_array_t* out_nodes);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_FUNC_LOCATION_CAPTURE_H_
