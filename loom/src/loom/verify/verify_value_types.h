// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_VERIFY_VERIFY_VALUE_TYPES_H_
#define LOOM_VERIFY_VERIFY_VALUE_TYPES_H_

#include "loom/verify/verify_state.h"

// Checks references carried by operation value types in the current scope.
void loom_verify_value_type_refs(loom_verify_state_t* state,
                                 const loom_op_t* op,
                                 const loom_op_vtable_t* vtable);

// Checks argument type references after the block's arguments are defined.
// The owner anchors diagnostics because block arguments have no locations.
void loom_verify_block_arg_type_refs(loom_verify_state_t* state,
                                     const loom_block_t* block,
                                     const loom_op_t* owner);

#endif  // LOOM_VERIFY_VERIFY_VALUE_TYPES_H_
