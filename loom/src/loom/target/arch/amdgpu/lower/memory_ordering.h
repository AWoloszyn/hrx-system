// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU ordering for scalar atomic observations, publications, and thread
// fences. These operations do not introduce an execution rendezvous.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_ORDERING_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_ORDERING_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/codegen/low/source_memory_plan.h"
#include "loom/target/low_legality.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_amdgpu_memory_fence_plan_t {
  // Source memory ordering selected for the fence.
  uint8_t ordering;
  // Source synchronization domain selected for the fence.
  uint8_t scope;
} loom_amdgpu_memory_fence_plan_t;

// Supplies the admitted native visibility recipe and locality cost facts to
// the shared function planner.
loom_low_lower_visibility_model_t loom_amdgpu_memory_visibility_model(
    const loom_low_lower_context_t* context);

// Returns an empty string when a scalar atomic access has a complete native
// implementation, or the target constraint preventing its implementation.
iree_string_view_t loom_amdgpu_atomic_memory_rejection_key(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source);

// Emits ordering required before an atomic memory packet. Ordinary accesses
// and relaxed atomics have no prefix.
iree_status_t loom_amdgpu_emit_memory_ordering_prefix(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_source_memory_access_plan_t* source);

// Emits acquisition required after an atomic load packet.
iree_status_t loom_amdgpu_emit_memory_ordering_suffix(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_low_source_memory_access_plan_t* source);

// Selects an independently executable thread fence.
iree_status_t loom_amdgpu_select_memory_fence_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan);

// Emits a previously selected thread fence without a workgroup barrier.
iree_status_t loom_amdgpu_lower_memory_fence(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_fence_plan_t* plan);

// Verifies target support for an authored buffer.fence.
iree_status_t loom_amdgpu_low_legality_verify_memory_fence(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_ORDERING_H_
