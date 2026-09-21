// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_OPS_FUNC_LOCATION_H_
#define LOOM_OPS_FUNC_LOCATION_H_

#include "loom/ir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Encodes verified func.location nodes into arena-owned immutable bytes using
// loom/format/location.h. No source resolution or compiler pointer escapes into
// the value. Failure is limited to allocation and the format's 4 GiB size
// bound.
iree_status_t loom_func_location_encode(const loom_module_t* module,
                                        loom_parameterized_attr_array_t nodes,
                                        iree_arena_allocator_t* arena,
                                        iree_const_byte_span_t* out_bytes);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_FUNC_LOCATION_H_
