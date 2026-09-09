// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_FUNCTION_H_
#define LOOM_TARGET_ARCH_VM_FUNCTION_H_

#include "iree/io/stream.h"
#include "iree/vm/bytecode/wire/module.h"
#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Exact logical signature retained by module collection for function emission.
typedef struct loom_vm_function_signature_t {
  // Physical argument/result counts. Serialization assigns descriptor_base.
  iree_vm_bytecode_v0_signature_row_t row;
  // Borrowed source-ordered argument descriptors followed by result
  // descriptors.
  const iree_vm_bytecode_v0_signature_descriptor_row_t* fields;
} loom_vm_function_signature_t;

// Schedules and allocates one prepared VM function with the common frame
// builder, then appends its instruction stream exactly once. |out_row| receives
// its byte length and frame high waters; the module writer owns callable and
// section-relative offsets. Branches target block markers using signed word
// offsets patched after emission. The shared allocator owns edge and packet
// moves, including cycle temporaries. Common allocation repair materializes
// scalar spills; the scheduler's stack layout owns their byte offsets. Call
// snapshots follow that durable storage and never overlap it. All compiler
// scratch belongs to |request|'s arena. Structured frame errors are forwarded
// to its diagnostic emitter and terminate emission with a failure status.
// |function_ordinals_by_symbol| maps module symbol IDs to local function
// ordinals, with UINT16_MAX for symbols outside this emitted module.
iree_status_t loom_vm_function_emit(
    const loom_target_emit_request_t* request, loom_func_like_t function,
    const loom_target_facts_t* target_facts,
    const loom_vm_function_signature_t* signature,
    const uint16_t* function_ordinals_by_symbol, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_function_row_t* out_row);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_FUNCTION_H_
