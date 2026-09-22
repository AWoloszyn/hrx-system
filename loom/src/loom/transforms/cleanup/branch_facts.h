// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CLEANUP_BRANCH_FACTS_H_
#define LOOM_TRANSFORMS_CLEANUP_BRANCH_FACTS_H_

#include "loom/analysis/condition_facts.h"
#include "loom/rewrite/greedy.h"

#ifdef __cplusplus
extern "C" {
#endif

// Materializes branch-edge refinements and replaces dominated region uses
// before a greedy worklist iteration. The caller owns the rewriter, its facts,
// and the reusable condition query. Mutations contribute to |result| and set
// |out_changed|; the caller invalidates symbolic expressions after changes.
iree_status_t loom_branch_facts_materialize_region(
    loom_rewriter_t* rewriter, loom_condition_query_t* condition_query,
    loom_region_t* region, loom_greedy_rewrite_result_t* result,
    bool* out_changed);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_BRANCH_FACTS_H_
