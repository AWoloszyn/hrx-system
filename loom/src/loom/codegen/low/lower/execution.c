// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/execution.h"

#include "loom/codegen/low/lower/context.h"
#include "loom/util/cfg_loop_nest.h"
#include "loom/util/fact_cfg.h"

static iree_status_t loom_low_lower_calculate_block_execution_counts(
    loom_low_lower_context_t* context, loom_region_t* body,
    iree_arena_allocator_t* scratch_arena, uint64_t* counts, bool* out_exact) {
  *out_exact = false;
  if (!iree_any_bit_set(body->flags, LOOM_REGION_INSTANCE_FLAG_CFG)) {
    counts[0] = 1;
    *out_exact = true;
    return iree_ok_status();
  }
  const loom_value_fact_cfg_region_t* region =
      loom_value_fact_table_lookup_cfg_region(context->lowering.fact_table,
                                              body);
  const loom_cfg_loop_nest_t* loops = &region->loops;
  uint64_t* trip_counts = NULL;
  if (loops->loop_count > 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(scratch_arena, loops->loop_count,
                                  sizeof(*trip_counts), (void**)&trip_counts));
  }
  for (iree_host_size_t i = 0; i < loops->loop_count; ++i) {
    const loom_loop_recurrence_facts_t recurrence =
        loom_value_fact_induction_facts(context->lowering.fact_table,
                                        context->module,
                                        &region->inductions[i]);
    if (!recurrence.trip_count_known) {
      return iree_ok_status();
    }
    trip_counts[i] = recurrence.trip_count;
  }
  *out_exact = loom_cfg_loop_nest_calculate_block_execution_counts(
      loops, trip_counts, counts);
  return iree_ok_status();
}

iree_status_t loom_low_lower_source_block_execution_counts(
    loom_low_lower_context_t* context, const uint64_t** out_counts) {
  *out_counts = NULL;
  loom_low_lower_execution_counts_t* analysis =
      &context->lowering.function_analysis.execution_counts;
  if (analysis->initialized) {
    *out_counts = analysis->blocks;
    return iree_ok_status();
  }
  loom_region_t* body = loom_func_like_body(context->source_function);
  if (body == NULL || body->block_count == 0) {
    analysis->initialized = true;
    return iree_ok_status();
  }
  uint64_t* counts = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(&context->function_arena, body->block_count,
                                sizeof(*counts), (void**)&counts));
  iree_arena_allocator_t scratch_arena;
  iree_arena_initialize(context->module->arena.block_pool, &scratch_arena);
  bool exact = false;
  iree_status_t status = loom_low_lower_calculate_block_execution_counts(
      context, body, &scratch_arena, counts, &exact);
  iree_arena_deinitialize(&scratch_arena);
  if (iree_status_is_ok(status)) {
    analysis->blocks = exact ? counts : NULL;
    analysis->initialized = true;
    *out_counts = analysis->blocks;
  }
  return status;
}
