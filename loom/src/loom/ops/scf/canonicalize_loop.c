// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string.h>

#include "loom/ir/module.h"
#include "loom/ops/scf/canonicalize.h"
#include "loom/ops/scf/ops.h"
#include "loom/rewrite/rewriter.h"

static iree_status_t loom_scf_preserve_value_name(
    loom_module_t* module, loom_value_id_t old_value_id,
    loom_value_id_t new_value_id) {
  if (old_value_id == LOOM_VALUE_ID_INVALID ||
      new_value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  return loom_module_copy_value_name(module, old_value_id, new_value_id);
}

//===----------------------------------------------------------------------===//
// scf.for
//===----------------------------------------------------------------------===//

static bool loom_scf_for_has_zero_trip_count(loom_op_t* op,
                                             loom_rewriter_t* rewriter) {
  loom_value_facts_t lower_bound =
      loom_rewriter_value_facts(rewriter, loom_scf_for_lower_bound(op));
  loom_value_facts_t upper_bound =
      loom_rewriter_value_facts(rewriter, loom_scf_for_upper_bound(op));
  if (loom_value_facts_is_float(lower_bound) ||
      loom_value_facts_is_float(upper_bound)) {
    return false;
  }
  if (loom_value_facts_is_exact(lower_bound) &&
      loom_value_facts_is_exact(upper_bound) &&
      lower_bound.range_lo == upper_bound.range_lo) {
    return true;
  }

  loom_value_facts_t step =
      loom_rewriter_value_facts(rewriter, loom_scf_for_step(op));
  if (!loom_value_facts_is_positive(step)) {
    return false;
  }
  return lower_bound.range_lo >= upper_bound.range_hi;
}

static bool loom_scf_for_step_is_positive(loom_op_t* op,
                                          loom_rewriter_t* rewriter) {
  loom_value_facts_t step =
      loom_rewriter_value_facts(rewriter, loom_scf_for_step(op));
  return !loom_value_facts_is_float(step) && loom_value_facts_is_positive(step);
}

static bool loom_scf_for_has_single_trip_count(loom_op_t* op,
                                               loom_rewriter_t* rewriter) {
  loom_value_facts_t lower_bound =
      loom_rewriter_value_facts(rewriter, loom_scf_for_lower_bound(op));
  loom_value_facts_t upper_bound =
      loom_rewriter_value_facts(rewriter, loom_scf_for_upper_bound(op));
  loom_value_facts_t step =
      loom_rewriter_value_facts(rewriter, loom_scf_for_step(op));
  if (loom_value_facts_is_float(lower_bound) ||
      loom_value_facts_is_float(upper_bound) ||
      loom_value_facts_is_float(step) || !loom_value_facts_is_positive(step)) {
    return false;
  }

  if (lower_bound.range_hi >= upper_bound.range_lo) {
    return false;
  }
  int64_t next_iv_lower_bound = 0;
  if (!iree_checked_add_i64(lower_bound.range_lo, step.range_lo,
                            &next_iv_lower_bound)) {
    return false;
  }
  return next_iv_lower_bound >= upper_bound.range_hi;
}

static bool loom_scf_for_yields_loop_carried_args(loom_op_t* op) {
  loom_region_t* body = loom_scf_for_body(op);
  loom_op_t* yield = loom_scf_region_terminator(body);
  if (!yield) {
    return false;
  }

  loom_block_t* block = loom_region_entry_block(body);
  if (block->first_op != yield) {
    return false;
  }
  if (op->result_count == 0) {
    return true;
  }

  loom_value_slice_t iter_args = loom_scf_for_iter_args(op);
  loom_value_slice_t yielded_values = loom_scf_yield_values(yield);
  if (iter_args.count != op->result_count ||
      yielded_values.count != op->result_count) {
    return false;
  }
  if (block->arg_count < 1 + op->result_count) {
    return false;
  }
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (yielded_values.values[i] != loom_block_arg_id(block, 1 + i)) {
      return false;
    }
  }
  return true;
}

static iree_status_t loom_scf_for_inline_single_trip(
    loom_op_t* op, loom_rewriter_t* rewriter) {
  if (!loom_scf_for_has_single_trip_count(op, rewriter)) {
    return iree_ok_status();
  }

  loom_region_t* body = loom_scf_for_body(op);
  loom_op_t* yield = loom_scf_region_terminator(body);
  if (!yield) {
    return iree_ok_status();
  }

  loom_value_slice_t iter_args = loom_scf_for_iter_args(op);
  loom_value_slice_t yielded_values = loom_scf_yield_values(yield);
  if (iter_args.count != op->result_count ||
      yielded_values.count != op->result_count) {
    return iree_ok_status();
  }

  loom_block_t* block = loom_region_entry_block(body);
  if (block->arg_count < 1 + iter_args.count) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
      rewriter, loom_block_arg_id(block, 0), loom_scf_for_lower_bound(op)));
  for (uint16_t i = 0; i < iter_args.count; ++i) {
    IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
        rewriter, loom_block_arg_id(block, (uint16_t)(1 + i)),
        iter_args.values[i]));
  }

  IREE_RETURN_IF_ERROR(
      loom_scf_move_region_body_before_op(rewriter, body, yield, op));

  loom_value_id_t* replacements = NULL;
  if (op->result_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        rewriter->arena, op->result_count, sizeof(*replacements),
        (void**)&replacements));
    yielded_values = loom_scf_yield_values(yield);
    memcpy(replacements, yielded_values.values,
           (iree_host_size_t)op->result_count * sizeof(*replacements));
  }
  return loom_scf_replace_results_and_erase(op, rewriter, replacements,
                                            op->result_count);
}

static iree_status_t loom_scf_for_adjust_tied_results(
    loom_op_t* op, const uint16_t* result_map, const uint16_t* iter_arg_map,
    uint16_t old_iter_arg_offset, uint16_t new_iter_arg_offset,
    loom_rewriter_t* rewriter, loom_tied_result_t** out_tied_results,
    uint16_t* out_tied_result_count, bool* out_supported) {
  *out_tied_results = NULL;
  *out_tied_result_count = 0;
  *out_supported = true;
  if (op->tied_result_count == 0) {
    return iree_ok_status();
  }

  loom_tied_result_t* tied_results = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(rewriter->arena, op->tied_result_count,
                                sizeof(*tied_results), (void**)&tied_results));

  const loom_tied_result_t* old_tied_results = loom_op_tied_results(op);
  uint16_t tied_result_count = 0;
  for (uint16_t i = 0; i < op->tied_result_count; ++i) {
    loom_tied_result_t tied_result = old_tied_results[i];
    if (tied_result.result_index >= op->result_count) {
      *out_supported = false;
      return iree_ok_status();
    }
    if (tied_result.operand_index >= op->operand_count) {
      *out_supported = false;
      return iree_ok_status();
    }
    uint16_t new_result_index = result_map[tied_result.result_index];
    if (new_result_index == UINT16_MAX) {
      continue;
    }

    uint16_t new_operand_index = tied_result.operand_index;
    if (new_operand_index >= old_iter_arg_offset) {
      uint16_t old_iter_arg_index =
          (uint16_t)(new_operand_index - old_iter_arg_offset);
      if (old_iter_arg_index >= op->result_count) {
        *out_supported = false;
        return iree_ok_status();
      }
      uint16_t new_iter_arg_index = iter_arg_map[old_iter_arg_index];
      if (new_iter_arg_index == UINT16_MAX) {
        *out_supported = false;
        return iree_ok_status();
      }
      new_operand_index = (uint16_t)(new_iter_arg_offset + new_iter_arg_index);
    }

    tied_results[tied_result_count++] = (loom_tied_result_t){
        .result_index = new_result_index,
        .operand_index = new_operand_index,
        .has_type_change = tied_result.has_type_change,
    };
  }

  *out_tied_results = tied_results;
  *out_tied_result_count = tied_result_count;
  return iree_ok_status();
}

// Compacts loop-carried slots whose yield operand is exactly the corresponding
// iter_arg block argument. The loop is rebuilt because scf.for operands, body
// block arguments, results, and tied-result metadata all share the carried slot
// numbering.
static iree_status_t loom_scf_for_forward_loop_carried_results(
    loom_op_t* op, loom_rewriter_t* rewriter) {
  if (!loom_scf_for_step_is_positive(op, rewriter) || op->result_count == 0) {
    return iree_ok_status();
  }

  loom_region_t* body = loom_scf_for_body(op);
  loom_op_t* yield = loom_scf_region_terminator(body);
  if (!yield) {
    return iree_ok_status();
  }

  loom_value_slice_t iter_args = loom_scf_for_iter_args(op);
  loom_value_slice_t yielded_values = loom_scf_yield_values(yield);
  if (iter_args.count != op->result_count ||
      yielded_values.count != op->result_count) {
    return iree_ok_status();
  }

  loom_scf_for_build_flags_t build_flags = 0;
  loom_value_id_t unroll_factor = LOOM_VALUE_ID_INVALID;
  bool has_unroll_factor = loom_scf_for_unroll_factor_is_present(op);
  if (has_unroll_factor) {
    build_flags |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR;
    unroll_factor = loom_scf_for_unroll_factor(op);
  }
  loom_scf_for_unroll_policy_t unroll_policy = 0;
  if (!loom_attr_is_absent(
          loom_op_attrs(op)[loom_scf_for_unroll_policy_ATTR_INDEX])) {
    build_flags |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY;
    unroll_policy = loom_scf_for_unroll_policy(op);
  }
  loom_scf_for_unroll_schedule_t unroll_schedule = 0;
  if (!loom_attr_is_absent(
          loom_op_attrs(op)[loom_scf_for_unroll_schedule_ATTR_INDEX])) {
    build_flags |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_SCHEDULE;
    unroll_schedule = loom_scf_for_unroll_schedule(op);
  }
  uint16_t old_iter_arg_offset =
      (uint16_t)(iter_args.values - loom_op_const_operands(op));
  uint16_t new_iter_arg_offset = 3;

  loom_block_t* old_block = loom_region_entry_block(body);
  if (old_block->arg_count < 1 + op->result_count) {
    return iree_ok_status();
  }

  bool* forwarded_results = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      rewriter->arena, op->result_count, sizeof(*forwarded_results),
      (void**)&forwarded_results));
  uint16_t forwarded_count = 0;
  for (uint16_t i = 0; i < op->result_count; ++i) {
    loom_value_id_t carried_arg =
        loom_block_arg_id(old_block, (uint16_t)(1 + i));
    forwarded_results[i] = yielded_values.values[i] == carried_arg;
    if (forwarded_results[i]) {
      ++forwarded_count;
    }
  }
  if (forwarded_count == 0) {
    return iree_ok_status();
  }

  uint16_t kept_count = (uint16_t)(op->result_count - forwarded_count);
  uint16_t* result_map = NULL;
  uint16_t* iter_arg_map = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(rewriter->arena, op->result_count,
                                sizeof(*result_map), (void**)&result_map));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(rewriter->arena, op->result_count,
                                sizeof(*iter_arg_map), (void**)&iter_arg_map));
  for (uint16_t i = 0; i < op->result_count; ++i) {
    result_map[i] = UINT16_MAX;
    iter_arg_map[i] = UINT16_MAX;
  }

  loom_value_id_t* kept_iter_args = NULL;
  loom_value_id_t* kept_yielded_values = NULL;
  if (kept_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(rewriter->arena, kept_count,
                                                   sizeof(*kept_iter_args),
                                                   (void**)&kept_iter_args));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        rewriter->arena, kept_count, sizeof(*kept_yielded_values),
        (void**)&kept_yielded_values));
  }

  uint16_t kept_ordinal = 0;
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (forwarded_results[i]) {
      continue;
    }
    result_map[i] = kept_ordinal;
    iter_arg_map[i] = kept_ordinal;
    kept_iter_args[kept_ordinal] = iter_args.values[i];
    kept_yielded_values[kept_ordinal] = yielded_values.values[i];
    ++kept_ordinal;
  }

  loom_tied_result_t* tied_results = NULL;
  uint16_t tied_result_count = 0;
  bool tied_results_supported = false;
  IREE_RETURN_IF_ERROR(loom_scf_for_adjust_tied_results(
      op, result_map, iter_arg_map, old_iter_arg_offset, new_iter_arg_offset,
      rewriter, &tied_results, &tied_result_count, &tied_results_supported));
  if (!tied_results_supported) {
    return iree_ok_status();
  }

  loom_builder_set_before(&rewriter->builder, op);
  loom_value_id_t value_checkpoint = loom_rewriter_value_checkpoint(rewriter);
  loom_value_id_t pipeline_depth = LOOM_VALUE_ID_INVALID;
  if (loom_scf_for_pipeline_depth_is_present(op)) {
    build_flags |= LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH;
    pipeline_depth = loom_scf_for_pipeline_depth(op);
  }
  loom_op_t* new_loop = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_for_build(
      &rewriter->builder, build_flags, loom_scf_for_lower_bound(op),
      loom_scf_for_upper_bound(op), loom_scf_for_step(op), kept_iter_args,
      kept_count, tied_results, tied_result_count, pipeline_depth,
      unroll_factor, unroll_policy, unroll_schedule, op->location, &new_loop));

  loom_region_t* new_body = loom_scf_for_body(new_loop);
  loom_builder_ip_t saved_ip =
      loom_builder_enter_region(&rewriter->builder, new_loop, new_body);
  loom_op_t* new_yield = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_yield_build(&rewriter->builder,
                                            kept_yielded_values, kept_count,
                                            op->location, &new_yield));
  loom_builder_restore(&rewriter->builder, saved_ip);

  loom_block_t* new_block = loom_region_entry_block(new_body);
  IREE_RETURN_IF_ERROR(loom_scf_preserve_value_name(
      rewriter->module, loom_block_arg_id(old_block, 0),
      loom_block_arg_id(new_block, 0)));
  kept_ordinal = 0;
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (forwarded_results[i]) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_scf_preserve_value_name(
        rewriter->module, loom_block_arg_id(old_block, (uint16_t)(1 + i)),
        loom_block_arg_id(new_block, (uint16_t)(1 + kept_ordinal++))));
  }

  IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
      rewriter, loom_block_arg_id(old_block, 0),
      loom_block_arg_id(new_block, 0)));
  kept_ordinal = 0;
  for (uint16_t i = 0; i < op->result_count; ++i) {
    loom_value_id_t old_arg = loom_block_arg_id(old_block, (uint16_t)(1 + i));
    loom_value_id_t replacement =
        forwarded_results[i]
            ? iter_args.values[i]
            : loom_block_arg_id(new_block, (uint16_t)(1 + kept_ordinal++));
    IREE_RETURN_IF_ERROR(
        loom_rewriter_replace_all_uses_with(rewriter, old_arg, replacement));
  }

  loom_op_t* child_op = old_block->first_op;
  while (child_op && child_op != yield) {
    loom_op_t* next_child_op = child_op->next_op;
    IREE_RETURN_IF_ERROR(
        loom_rewriter_move_before(rewriter, child_op, new_yield));
    child_op = next_child_op;
  }

  loom_value_id_t* replacements = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(rewriter->arena, op->result_count,
                                sizeof(*replacements), (void**)&replacements));
  kept_ordinal = 0;
  loom_value_slice_t new_results = loom_scf_for_results(new_loop);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    replacements[i] = forwarded_results[i] ? iter_args.values[i]
                                           : new_results.values[kept_ordinal++];
  }
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, replacements, op->result_count, value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, replacements,
                                                  op->result_count);
}

iree_status_t loom_scf_for_canonicalize(loom_op_t* op,
                                        loom_rewriter_t* rewriter) {
  loom_value_slice_t iter_args = loom_scf_for_iter_args(op);
  if (loom_scf_for_has_zero_trip_count(op, rewriter)) {
    return loom_scf_replace_results_and_erase(op, rewriter, iter_args.values,
                                              iter_args.count);
  }
  IREE_RETURN_IF_ERROR(loom_scf_for_inline_single_trip(op, rewriter));
  if (op->flags & LOOM_OP_FLAG_DEAD) {
    return iree_ok_status();
  }
  if (loom_scf_for_step_is_positive(op, rewriter) &&
      loom_scf_for_yields_loop_carried_args(op)) {
    return loom_scf_replace_results_and_erase(op, rewriter, iter_args.values,
                                              iter_args.count);
  }
  return loom_scf_for_forward_loop_carried_results(op, rewriter);
}
