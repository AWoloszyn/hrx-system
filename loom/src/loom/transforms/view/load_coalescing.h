// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_VIEW_LOAD_COALESCING_H_
#define LOOM_TRANSFORMS_VIEW_LOAD_COALESCING_H_

#include "loom/analysis/symbolic_expr.h"
#include "loom/rewrite/rewriter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Combines consecutive 16-bit scalar loads from adjacent coordinates of the
// same view into one vector load with scalar extracts. Matching cache policies
// and exact symbolic adjacency preserve the accesses and their program order.
// The caller supplies a view.load op, owns traversal, facts and symbolic
// scratch, and resets the symbolic context after a rewrite. This optimization
// introduces vector representations and therefore belongs before target
// legalization, not in final cleanup.
iree_status_t loom_view_load_coalescing_rewrite(
    loom_rewriter_t* rewriter, loom_symbolic_expr_context_t* expression_context,
    loom_op_t* op, bool* out_changed);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_VIEW_LOAD_COALESCING_H_
