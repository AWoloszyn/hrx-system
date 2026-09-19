// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_TYPE_IMPORT_H_
#define LOOM_IR_TYPE_IMPORT_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Imports a function, dialect or typed-register graph into canonical module
// storage. Each distinct temporary compound payload is processed once.
// Temporary identity and explicit traversal frames live only for this call in
// pooled scratch.
iree_status_t loom_type_import(loom_module_t* module, loom_type_t type,
                               loom_type_id_t* out_type_id);

// Imports a function signature supplied as separate temporary child spans.
iree_status_t loom_type_import_function(loom_module_t* module,
                                        const loom_type_t* arg_types,
                                        uint16_t arg_count,
                                        const loom_type_t* result_types,
                                        uint16_t result_count,
                                        loom_type_id_t* out_type_id);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_IR_TYPE_IMPORT_H_
