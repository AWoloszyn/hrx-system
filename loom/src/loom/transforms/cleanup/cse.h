// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CSE_H_
#define LOOM_TRANSFORMS_CSE_H_

#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns immutable metadata for the common subexpression elimination pass.
const loom_pass_info_t* loom_cse_pass_info(void);

// Common subexpression elimination pass.
//
// Iterative DFS over the region nesting tree with a scope chain of
// hash tables. For each eligible op (has results, no regions, no
// writes/unknown-effects/non-determinism), computes a content-aware
// hash and queries visible scope tables for a structurally equivalent op.
// If found, replaces all uses and erases the duplicate.
//
// Key properties:
//   - Iterative (no recursion): bounded stack usage via an explicit
//     DFS frame stack, safe on arbitrarily nested input.
//   - Tombstone-based write barriers: PURE ops survive writes, non-PURE
//     ops (reads) are invalidated without breaking probe chains.
//   - Convergent barriers clear occupied slots, including tombstones. Total
//     full-table invalidation work is bounded by block insertions, with one
//     retained slot index per possible insertion.
//   - Full and read barriers use path-compressed ancestor indexes to skip
//     scopes with no pending entries, with logarithmic amortized query cost.
//     Insertion rearms the active block after nested regions finish.
//   - CFG dominance: each multiblock region's graph is built once. Scope
//     construction consumes its indexed immediate dominators directly.
//     Stateful lookup stops at joins and backedges; pure candidates remain
//     visible in dominated blocks.
//   - Local-table misses use a copy-on-write hash radix to visit only ancestor
//     tables with live candidates of that hash. Radix paths are bounded by
//     the 32-bit hash width; structural collisions still use table equality.
//     Index publication is lazy, so childless blocks keep the local fast path.
//     Permanently expired insertion identities use path-compressed links;
//     slot reuse cannot revive stale candidates in shared scope snapshots.
//   - Target-state dependencies are indexed at insertion by register class.
//     State writes query only their classes and expire visible memberships,
//     without scanning unrelated candidates or ancestor scopes. This shares
//     the scoped radix and expiration contract with the hash index; state keys
//     use at most six discriminator bits. Ordinary entries carry no state mask.
//   - Deep attribute comparison: pointer-valued attribute kinds
//     (I64_ARRAY, PREDICATE_LIST, DICT) are compared by content via
//     loom_attribute_equal, not by pointer identity.
//   - Split arena strategy: scope tables live in a dedicated arena
//     reset between function-like root regions; the DFS stack lives in the
//     pass arena. Peak scope memory is bounded by the largest root region.
iree_status_t loom_cse_run(loom_pass_t* pass, loom_module_t* module,
                           loom_func_like_t function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CSE_H_
