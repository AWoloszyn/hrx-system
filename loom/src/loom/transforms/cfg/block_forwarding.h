// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Resolves empty CFG forwarding chains before rewriting their incoming edges.

#ifndef LOOM_TRANSFORMS_CFG_BLOCK_FORWARDING_H_
#define LOOM_TRANSFORMS_CFG_BLOCK_FORWARDING_H_

#include "loom/rewrite/rewriter.h"
#include "loom/util/cfg_graph.h"
#include "loom/util/dominance.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolves argument-free forwarding chains in O(B + E) graph work. Pure
// forwarding cycles retain one self-loop, and conditional edges retain any
// block needed to supply a branch payload. Direct edges can take that payload
// when its values and dependent types are available at the incoming branch.
// Argument-bearing blocks are handled by single-predecessor fusion.
//
// All decisions precede mutation. On success, |out_forwarded_count| reports
// changed edges; a nonzero result completes one structural edit. The caller
// refreshes CFG facts before draining the rewriter worklist or querying
// analyses again. |graph| and |dominance| stay live throughout this call.
iree_status_t loom_cfg_forward_empty_blocks(
    loom_rewriter_t* rewriter, const loom_cfg_graph_t* graph,
    const loom_dominance_info_t* dominance, iree_arena_allocator_t* arena,
    iree_host_size_t* out_forwarded_count);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CFG_BLOCK_FORWARDING_H_
