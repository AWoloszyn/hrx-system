// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Contracts single-predecessor chains using a current CFG snapshot.

#ifndef LOOM_TRANSFORMS_CFG_BLOCK_FUSION_H_
#define LOOM_TRANSFORMS_CFG_BLOCK_FUSION_H_

#include "loom/rewrite/rewriter.h"
#include "loom/util/cfg_graph.h"
#include "loom/util/dominance.h"

#ifdef __cplusplus
extern "C" {
#endif

// Proves argument replacements before mutation, then contracts eligible chains
// in graph-owned reverse postorder and removes their empty blocks together.
// Every moved operation moves once, including along long chains whose source
// block order differs from control-flow order. Entry blocks and shared loop
// headers survive. |dominance| must cover |graph| and its ancestor regions.
//
// On success, |out_fused_count| reports the number of removed blocks. A nonzero
// result completes one structural edit: the caller refreshes CFG facts before
// draining the rewriter worklist or making any further analysis query. Snapshot
// memory remains caller-owned and must stay live throughout the edit.
iree_status_t loom_cfg_fuse_single_predecessor_blocks(
    loom_rewriter_t* rewriter, const loom_cfg_graph_t* graph,
    const loom_dominance_info_t* dominance, iree_arena_allocator_t* arena,
    uint16_t* out_fused_count);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CFG_BLOCK_FUSION_H_
