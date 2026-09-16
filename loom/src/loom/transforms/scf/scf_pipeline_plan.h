// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Author-requested ordinary read-ahead schedules.

#ifndef LOOM_TRANSFORMS_SCF_SCF_PIPELINE_PLAN_H_
#define LOOM_TRANSFORMS_SCF_SCF_PIPELINE_PLAN_H_

#include "loom/transforms/scf/scf_body.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t loom_scf_pipeline_stage_t;
enum loom_scf_pipeline_stage_e {
  LOOM_SCF_PIPELINE_STAGE_PRODUCER = 0,
  LOOM_SCF_PIPELINE_STAGE_CONSUMER = 1,
};

typedef struct loom_scf_pipeline_plan_t {
  // Complete source operations and local payload dependencies.
  loom_scf_body_t body;
  // Stage of each body operation, in authored order.
  loom_scf_pipeline_stage_t* stages;
  // Source values carried from a producer iteration to its consumer.
  loom_value_id_t* queue_values;
  // Number of values in each queued iteration record.
  uint32_t queue_value_count;
  // Number of static ordinary load operations, including nested regions.
  uint32_t read_count;
} loom_scf_pipeline_plan_t;

typedef struct loom_scf_pipeline_rejection_t {
  // Source operation preventing the requested schedule, or NULL on success.
  const loom_op_t* op;
  // Structural requirement violated by that operation.
  iree_string_view_t constraint;
} loom_scf_pipeline_rejection_t;

// Builds the two-stage read-ahead schedule of a verified loop body.
// Ordinary reads and their transitive payload prerequisites form the producer;
// other operations and the carried recurrence form the ordered consumer.
// Nested scf.if/scf.for operations remain intact within their assigned stage.
// The plan owns the complete cut, including values referenced only by types
// or attributes. Materializers and reports consume this cut directly.
//
// The admission contract excludes other nested control, ordered effects,
// writes, producers depending on carried state, and cross-stage values whose
// types vary with the iteration. Rejections identify the source requirement;
// status failures identify allocation or representation limits. Plan storage
// belongs to |arena| and borrows the source IR until reconstruction completes.
iree_status_t loom_scf_pipeline_plan_build(
    loom_module_t* module, const loom_block_t* block,
    iree_arena_allocator_t* arena, loom_scf_pipeline_plan_t* out_plan,
    loom_scf_pipeline_rejection_t* out_rejection);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_SCF_SCF_PIPELINE_PLAN_H_
