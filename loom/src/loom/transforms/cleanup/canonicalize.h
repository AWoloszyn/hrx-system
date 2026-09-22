// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CLEANUP_CANONICALIZE_H_
#define LOOM_TRANSFORMS_CLEANUP_CANONICALIZE_H_

#include "loom/pass/types.h"

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Pass wrapper
//===----------------------------------------------------------------------===//

// Returns immutable metadata for the canonicalize pass.
const loom_pass_info_t* loom_canonicalize_pass_info(void);

// Creates canonicalize pass state from a textual option dictionary.
iree_status_t loom_canonicalize_create(loom_pass_t* pass,
                                       iree_string_view_t options);

// Resolves pass-scoped target facts and math policy, then runs the reusable
// canonicalizer to a fixed point and records its changes and statistics.
iree_status_t loom_canonicalize_run(loom_pass_t* pass, loom_module_t* module,
                                    loom_func_like_t function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_CLEANUP_CANONICALIZE_H_
