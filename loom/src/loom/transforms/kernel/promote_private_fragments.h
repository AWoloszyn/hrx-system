// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Promotes invocation-private scalar cells and fragment buffers into SSA.
//
// Allocation-wide byte-region and value-flow planning replaces scalar storage
// with ordinary control-flow results and arguments. Aliases to the same typed
// cell share state; disjoint fields retain independent state. Unknown
// observers, incompatible overlaps, and observable accesses keep the allocation
// in memory.
//
// Full-domain scalar copy loops into rank-1 private views additionally become
// vector loads with scalar extracts. Both mechanisms preserve frontend intent
// in shared IR before target lowering.

#ifndef LOOM_TRANSFORMS_PROMOTE_PRIVATE_FRAGMENTS_H_
#define LOOM_TRANSFORMS_PROMOTE_PRIVATE_FRAGMENTS_H_

#include "iree/base/api.h"
#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

const loom_pass_info_t* loom_promote_private_fragments_pass_info(void);

iree_status_t loom_promote_private_fragments_run(loom_pass_t* pass,
                                                 loom_module_t* module,
                                                 loom_func_like_t function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_PROMOTE_PRIVATE_FRAGMENTS_H_
