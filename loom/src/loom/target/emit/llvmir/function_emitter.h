// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Function-local LLVMIR SSA, type, ABI, and control-flow emission.

#ifndef LOOM_TARGET_EMIT_LLVMIR_FUNCTION_EMITTER_H_
#define LOOM_TARGET_EMIT_LLVMIR_FUNCTION_EMITTER_H_

#include "loom/codegen/low/target_binding.h"
#include "loom/error/emitter.h"
#include "loom/ir/local_value_domain.h"
#include "loom/target/emit/llvmir/builder.h"
#include "loom/target/emit/llvmir/target_env.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_LLVMIR_LOW_EMITTER_KEY IREE_SV("llvmir.low")

typedef enum loom_llvmir_emit_core_type_e {
  LOOM_LLVMIR_EMIT_CORE_TYPE_I1 = 0,
  LOOM_LLVMIR_EMIT_CORE_TYPE_I8 = 1,
  LOOM_LLVMIR_EMIT_CORE_TYPE_I16 = 2,
  LOOM_LLVMIR_EMIT_CORE_TYPE_I32 = 3,
  LOOM_LLVMIR_EMIT_CORE_TYPE_I64 = 4,
  LOOM_LLVMIR_EMIT_CORE_TYPE_F16 = 5,
  LOOM_LLVMIR_EMIT_CORE_TYPE_BF16 = 6,
  LOOM_LLVMIR_EMIT_CORE_TYPE_F32 = 7,
  LOOM_LLVMIR_EMIT_CORE_TYPE_F64 = 8,
  LOOM_LLVMIR_EMIT_CORE_TYPE_PTR = 9,
} loom_llvmir_emit_core_type_t;

// Emission state for one immutable, verified target-low function. The module
// emitter resolves the target/profile and supplies the destination module;
// function emission owns the value domain and block/phi correspondence.
typedef struct loom_llvmir_emit_function_state_t {
  // Diagnostic sink borrowed from the module-emission call.
  iree_diagnostic_emitter_t diagnostic_emitter;
  // Number of target-support errors reported while emitting this function.
  iree_host_size_t error_count;
  // Module containing the emitted low function.
  loom_module_t* module;
  // Target-low function definition being emitted.
  loom_op_t* function_op;
  // Target-low function body being emitted.
  const loom_region_t* body;
  // Resolved target record and descriptor set for |function_op|.
  const loom_low_resolved_target_t* target;
  // Function-local target profile storage derived from |target|.
  loom_llvmir_target_profile_storage_t target_profile_storage;
  // Function-local LLVMIR target profile derived from |target_profile_storage|.
  const loom_llvmir_target_profile_t* target_profile;
  // Function symbol name used in diagnostics.
  iree_string_view_t function_name;
  // Scratch arena borrowed from the module-emission call.
  iree_arena_allocator_t* scratch_arena;
  // Structured LLVMIR module being built.
  loom_llvmir_module_t* llvmir_module;
  // Structured LLVMIR function being built.
  loom_llvmir_function_t* llvmir_function;
  // Structured LLVMIR block currently receiving instructions.
  loom_llvmir_block_t* llvmir_block;
  // LLVMIR blocks indexed by the shared CFG's stable region block ordinal.
  loom_llvmir_block_t** block_map;
  // Acquired function-local ordinal domain for the immutable Low body.
  loom_local_value_domain_t value_domain;
  // LLVMIR values indexed by function-local value ordinal.
  loom_llvmir_value_id_t* value_map;
  // Number of LLVMIR return values expected by this function.
  uint16_t result_count;
} loom_llvmir_emit_function_state_t;

// Returns the declared LLVMIR identity of a dominating or phi value.
static inline loom_llvmir_value_id_t loom_llvmir_emit_lookup_value(
    const loom_llvmir_emit_function_state_t* state, loom_value_id_t value_id) {
  const loom_llvmir_value_id_t value =
      state->value_map[loom_local_value_domain_ordinal(&state->value_domain,
                                                       value_id)];
  // Phi identities are predeclared and other definitions precede their users
  // in the shared CFG traversal. A missing entry violates that lifecycle.
  IREE_ASSERT_NE(value, LOOM_LLVMIR_VALUE_ID_INVALID);
  return value;
}

// Records a definition in the acquired function-local value domain.
static inline void loom_llvmir_emit_define_value(
    loom_llvmir_emit_function_state_t* state, loom_value_id_t value_id,
    loom_llvmir_value_id_t llvmir_value_id) {
  state->value_map[loom_local_value_domain_ordinal(&state->value_domain,
                                                   value_id)] = llvmir_value_id;
}

// Matches a descriptor against one instruction family table.
static inline bool loom_llvmir_emit_descriptor_ref_in(
    uint32_t descriptor_ref, const uint32_t* descriptor_refs,
    iree_host_size_t descriptor_ref_count) {
  for (iree_host_size_t i = 0; i < descriptor_ref_count; ++i) {
    if (descriptor_refs[i] == descriptor_ref) {
      return true;
    }
  }
  return false;
}

// Returns the retained source name for a Low value.
iree_string_view_t loom_llvmir_emit_value_name(const loom_module_t* module,
                                               loom_value_id_t value_id);

// Reports unsupported target behavior and records a function error.
iree_status_t loom_llvmir_emit_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    const loom_error_def_t* error, const loom_diagnostic_param_t* params,
    iree_host_size_t param_count);

// Reports a target shape unsupported by this emitter.
iree_status_t loom_llvmir_emit_shape_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    iree_string_view_t subject_kind, uint32_t actual_count,
    uint32_t expected_count);

// Reports a value representation unsupported by the target.
iree_status_t loom_llvmir_emit_value_type_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    iree_string_view_t expected_constraint);

// Reports a descriptor without a target emission implementation.
iree_status_t loom_llvmir_emit_unsupported_descriptor_diagnostic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet);

// Reads a required integer immediate, diagnosing an unsupported encoding.
iree_status_t loom_llvmir_emit_read_i64_immediate(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    iree_string_view_t immediate_name, bool* out_present, int64_t* out_value);

// Reads an optional integer immediate, preserving the default when absent.
iree_status_t loom_llvmir_emit_read_optional_i64_immediate(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    iree_string_view_t immediate_name, int64_t* inout_value);

// Interns the scalar LLVMIR type for a selected register class.
iree_status_t loom_llvmir_emit_core_scalar_type(
    loom_llvmir_module_t* module, loom_llvmir_emit_core_type_t type,
    uint32_t pointer_address_space, loom_llvmir_type_id_t* out_type_id);

// Interns the scalar or vector LLVMIR type for a selected register class.
iree_status_t loom_llvmir_emit_core_type(loom_llvmir_module_t* module,
                                         loom_llvmir_emit_core_type_t type,
                                         uint32_t unit_count,
                                         uint32_t pointer_address_space,
                                         loom_llvmir_type_id_t* out_type_id);

// Returns whether the selected value has the scalar pointer register class.
bool loom_llvmir_emit_low_value_is_pointer_register(
    loom_llvmir_emit_function_state_t* state, loom_value_id_t value_id);

// Interns a selected Low value type, preserving its pointer address space.
iree_status_t loom_llvmir_emit_type_for_low_value(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    loom_diagnostic_field_ref_t field_ref, loom_llvmir_type_id_t* out_type_id);

// Resolves the result value and LLVMIR type of a one-result packet.
iree_status_t loom_llvmir_emit_prepare_packet_result(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_type_id_t* out_result_type, loom_value_id_t* out_result_value);

// Returns the lane count retained in a selected Low register type.
uint32_t loom_llvmir_emit_low_value_unit_count(
    loom_llvmir_emit_function_state_t* state, loom_value_id_t value_id);

// Interns an i64 constant in the destination module.
iree_status_t loom_llvmir_emit_i64_constant(
    loom_llvmir_emit_function_state_t* state, int64_t value,
    loom_llvmir_value_id_t* out_value);

// Emits the function into the supplied module and releases its value domain.
// Allocation and diagnostic-sink failures propagate as status. Unsupported
// target behavior increments error_count; the module owner discards the output.
iree_status_t loom_llvmir_emit_function(
    loom_llvmir_emit_function_state_t* state);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_LLVMIR_FUNCTION_EMITTER_H_
