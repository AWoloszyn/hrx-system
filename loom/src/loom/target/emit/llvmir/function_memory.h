// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// LLVMIR memory instruction emission.

#ifndef LOOM_TARGET_EMIT_LLVMIR_FUNCTION_MEMORY_H_
#define LOOM_TARGET_EMIT_LLVMIR_FUNCTION_MEMORY_H_

#include "loom/target/emit/llvmir/function_emitter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits the packet when its descriptor belongs to this instruction family.
// Sets out_matched even when emission reports a target-support diagnostic;
// unmatched packets leave function state unchanged. Status carries allocation
// or diagnostic-sink failure.
iree_status_t loom_llvmir_emit_memory_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet, bool* out_matched);

// Resolves the retained resource or scratch memory space of a Low pointer.
iree_status_t loom_llvmir_emit_pointer_address_space_for_low_value(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    iree_string_view_t expected_constraint, uint32_t* out_address_space,
    bool* out_supported);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_LLVMIR_FUNCTION_MEMORY_H_
