// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_MODULE_H_
#define LOOM_TARGET_ARCH_VM_MODULE_H_

#include "loom/target/arch/vm/function.h"
#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Module-owned function definitions shared by table and instruction emission.
typedef struct loom_vm_module_function_t {
  // Borrowed executable function and signature values.
  loom_func_like_t function;
  // Function target facts retained by the shared specialization pipeline.
  const loom_target_facts_t* target_facts;
  // Source-ordered entry argument IDs in the module.
  const loom_value_id_t* arguments;
  // Source-ordered signature result IDs in the module.
  loom_value_slice_t results;
  // Public name, or empty for an internal function.
  iree_string_view_t export_name;
  // Function ordinal in the emitted image.
  uint16_t ordinal;
  // Source-ordered logical argument count.
  uint16_t argument_count;
  // Canonical callable ordinal assigned by signature sorting.
  uint16_t callable_ordinal;
  // Exact logical fields and their physical argument/result bank counts.
  loom_vm_function_signature_t signature;
} loom_vm_module_function_t;

// Module-local emission plan. Function ordinals come from the symbol walk;
// data ordinals are assigned on first emitted use, excluding other targets'
// payloads without a second traversal of function bodies.
typedef struct loom_vm_module_plan_t {
  // Arena-owned function records in bytecode ordinal order.
  loom_vm_module_function_t* values;
  // Symbol-indexed ordinals in the definition's function or rodata table.
  // UINT16_MAX marks definitions not emitted by this module writer.
  uint16_t* ordinals_by_symbol;
  // Number of records in |values|, bounded by the module symbol ID space.
  uint32_t count;
  // Read-only payloads retained for the module's data section.
  struct {
    // Module symbol IDs in declaration order for numeric Low operands.
    const loom_symbol_id_t* symbols;
    // Number of entries in |symbols|.
    uint32_t symbol_count;
    // Borrowed definitions in first-use order, with symbol_count capacity.
    const loom_op_t** values;
    // Number of definitions in |values|.
    uint32_t count;
    // Maximum block alignment, at least the image's eight-byte alignment.
    uint32_t alignment;
  } rodata;
  // Whether any signature names the Core buffer reference type.
  bool uses_buffer_type;
} loom_vm_module_plan_t;

// Emits VM functions in a prepared mixed-target module as one immutable .vm
// artifact. Signature and export tables are sorted for runtime consumption;
// the common compiler has already resolved the functions participating in the
// module. Referenced read-only payloads retain their source alignment and map
// to module-owned immutable buffers. Bytes are appended once to a segmented
// stream and fixed table rows are backpatched. No instruction sizing pass or
// contiguous image is required. Success transfers the byte sequence to
// |out_artifact|; failure publishes none.
iree_status_t loom_vm_module_emit(const loom_target_emit_request_t* request,
                                  loom_target_emit_artifact_t* out_artifact);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_MODULE_H_
