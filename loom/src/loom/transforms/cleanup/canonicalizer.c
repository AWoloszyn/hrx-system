// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cleanup/canonicalizer.h"

#include <string.h>

#include "loom/analysis/condition_facts.h"
#include "loom/analysis/symbolic_expr.h"
#include "loom/analysis/symbolic_expr_proof.h"
#include "loom/ir/context.h"
#include "loom/ir/facts.h"
#include "loom/ir/module.h"
#include "loom/ops/index/compare.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/special_values.h"
#include "loom/ops/type_registry.h"
#include "loom/ops/vector/ops.h"
#include "loom/ops/vector/table.h"
#include "loom/ops/view/ops.h"
#include "loom/pass/value_facts.h"
#include "loom/rewrite/greedy.h"
#include "loom/rewrite/rewriter.h"
#include "loom/rewrite/type_propagation.h"
#include "loom/transforms/cleanup/branch_facts.h"
#include "loom/transforms/view/load_coalescing.h"

static iree_status_t loom_canonicalize_replace_single_result_with_value(
    loom_rewriter_t* rewriter, loom_op_t* op, loom_value_id_t replacement) {
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement,
                                                  1);
}

static iree_status_t loom_canonicalize_replace_single_result_with_exact_i64(
    loom_rewriter_t* rewriter, loom_op_t* op, int64_t value) {
  loom_builder_set_before(&rewriter->builder, op);
  loom_value_id_t value_checkpoint = loom_rewriter_value_checkpoint(rewriter);
  loom_value_id_t result = loom_op_const_results(op)[0];
  loom_type_t result_type = loom_module_value_type(rewriter->module, result);

  loom_value_id_t replacement = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_rewriter_build_constant(rewriter, loom_value_facts_exact_i64(value),
                                   result_type, op->location, &replacement));
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  return loom_canonicalize_replace_single_result_with_value(rewriter, op,
                                                            replacement);
}

//===----------------------------------------------------------------------===//
// Implementation
//===----------------------------------------------------------------------===//

static bool loom_canonicalize_value_type_has_poison(loom_type_t type) {
  if (loom_type_is_scalar(type)) {
    return true;
  }
  if (loom_type_is_vector(type)) {
    return !loom_type_has_static_zero_extent(type);
  }
  return false;
}

static bool loom_canonicalize_type_is_static_empty_vector(loom_type_t type) {
  return loom_type_is_vector(type) && loom_type_has_static_zero_extent(type);
}

static bool loom_canonicalize_value_is_static_empty_vector(
    const loom_module_t* module, loom_value_id_t value_id) {
  if (value_id == LOOM_VALUE_ID_INVALID || value_id >= module->values.count) {
    return false;
  }
  return loom_canonicalize_type_is_static_empty_vector(
      loom_module_value_type(module, value_id));
}

static bool loom_canonicalize_op_has_poison_operand(const loom_module_t* module,
                                                    const loom_op_t* op) {
  const loom_value_id_t* operands = loom_op_const_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    if (operands[i] == LOOM_VALUE_ID_INVALID) {
      continue;
    }
    if (loom_value_is_poison(module, operands[i])) {
      return true;
    }
  }
  return false;
}

static bool loom_canonicalize_can_replace_results_with_poison(
    const loom_module_t* module, const loom_op_t* op) {
  if (op->result_count == 0) {
    return false;
  }
  if (op->region_count != 0) {
    return false;
  }
  if (op->tied_result_count != 0) {
    return false;
  }
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (results[i] == LOOM_VALUE_ID_INVALID) {
      return false;
    }
    loom_type_t type = loom_module_value_type(module, results[i]);
    if (!loom_canonicalize_value_type_has_poison(type)) {
      return false;
    }
  }
  return true;
}

static iree_status_t loom_canonicalize_try_propagate_poison(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_propagated) {
  *out_propagated = false;
  if (!loom_module_has_poison(rewriter->module)) {
    return iree_ok_status();
  }
  loom_trait_flags_t traits = loom_op_effective_traits(rewriter->module, op);
  if (!iree_any_bit_set(traits, LOOM_TRAIT_PURE)) {
    return iree_ok_status();
  }
  if (loom_traits_are_convergent(traits)) {
    return iree_ok_status();
  }
  if (!loom_canonicalize_op_has_poison_operand(rewriter->module, op)) {
    return iree_ok_status();
  }
  if (!loom_canonicalize_can_replace_results_with_poison(rewriter->module,
                                                         op)) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_results_with_materialized_values_and_erase(
          rewriter, op, loom_poison_build));
  *out_propagated = true;
  return iree_ok_status();
}

static bool loom_canonicalize_extract_consumes_static_empty_axis(
    const loom_module_t* module, const loom_op_t* op) {
  if (!loom_vector_extract_isa(op)) {
    return false;
  }
  loom_type_t source_type =
      loom_module_value_type(module, loom_vector_extract_source(op));
  if (!loom_type_is_vector(source_type)) {
    return false;
  }

  loom_attribute_t static_indices = loom_vector_extract_static_indices(op);
  if (static_indices.kind != LOOM_ATTR_I64_ARRAY) {
    return false;
  }

  uint8_t source_rank = loom_type_rank(source_type);
  uint16_t consumed_rank = static_indices.count;
  if (consumed_rank > source_rank) {
    consumed_rank = source_rank;
  }
  for (uint16_t axis = 0; axis < consumed_rank; ++axis) {
    if (loom_type_dim_is_dynamic_at(source_type, axis)) {
      continue;
    }
    if (loom_type_dim_static_size_at(source_type, axis) == 0) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_canonicalize_try_replace_empty_extract_with_poison(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_replaced) {
  *out_replaced = false;
  if (!loom_canonicalize_extract_consumes_static_empty_axis(rewriter->module,
                                                            op)) {
    return iree_ok_status();
  }
  loom_value_id_t result = loom_vector_extract_result(op);
  if (result == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  loom_type_t result_type = loom_module_value_type(rewriter->module, result);
  if (!loom_canonicalize_value_type_has_poison(result_type)) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_results_with_materialized_values_and_erase(
          rewriter, op, loom_poison_build));
  *out_replaced = true;
  return iree_ok_status();
}

static iree_status_t loom_canonicalize_try_fold_empty_accumulator_op(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_folded) {
  *out_folded = false;

  loom_value_id_t input = LOOM_VALUE_ID_INVALID;
  loom_value_id_t init = LOOM_VALUE_ID_INVALID;
  if (loom_vector_reduce_isa(op)) {
    input = loom_vector_reduce_input(op);
    init = loom_vector_reduce_init(op);
  } else if (loom_vector_dotf_isa(op)) {
    input = loom_vector_dotf_lhs(op);
    init = loom_vector_dotf_init(op);
  } else {
    return iree_ok_status();
  }

  if (!loom_canonicalize_value_is_static_empty_vector(rewriter->module,
                                                      input)) {
    return iree_ok_status();
  }

  loom_value_id_t replacement = init;
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement, 1));
  *out_folded = true;
  return iree_ok_status();
}

static bool loom_canonicalize_can_replace_results_with_empty(
    const loom_module_t* module, const loom_op_t* op) {
  if (loom_op_is_empty(op) || loom_op_is_poison(op)) {
    return false;
  }
  if (op->result_count == 0) {
    return false;
  }
  if (op->region_count != 0) {
    return false;
  }
  if (op->tied_result_count != 0) {
    return false;
  }

  loom_trait_flags_t traits = loom_op_effective_traits(module, op);
  if (!iree_any_bit_set(traits, LOOM_TRAIT_PURE)) {
    return false;
  }
  if (loom_traits_are_convergent(traits)) {
    return false;
  }

  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (results[i] == LOOM_VALUE_ID_INVALID) {
      return false;
    }
    loom_type_t type = loom_module_value_type(module, results[i]);
    if (!loom_type_has_empty_materializer(type)) {
      return false;
    }
  }
  return true;
}

static bool loom_canonicalize_empty_value(const loom_module_t* module,
                                          loom_value_id_t value_id) {
  return loom_canonicalize_value_is_static_empty_vector(module, value_id);
}

static bool loom_canonicalize_optional_empty_value(const loom_module_t* module,
                                                   loom_value_id_t value_id) {
  return value_id == LOOM_VALUE_ID_INVALID ||
         loom_canonicalize_empty_value(module, value_id);
}

static bool loom_canonicalize_required_empty_value(const loom_module_t* module,
                                                   loom_value_id_t value_id) {
  return value_id != LOOM_VALUE_ID_INVALID &&
         loom_canonicalize_empty_value(module, value_id);
}

static bool loom_canonicalize_memory_access_results_are_empty(
    const loom_module_t* module, const loom_op_t* op) {
  if (op->result_count == 0) {
    return false;
  }
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (!loom_canonicalize_required_empty_value(module, results[i])) {
      return false;
    }
  }
  return true;
}

static bool loom_canonicalize_memory_access_optional_roles_are_empty(
    const loom_module_t* module, loom_memory_access_t access) {
  return loom_canonicalize_optional_empty_value(
             module, loom_memory_access_mask(access)) &&
         loom_canonicalize_optional_empty_value(
             module, loom_memory_access_passthrough(access)) &&
         loom_canonicalize_optional_empty_value(
             module, loom_memory_access_offsets(access));
}

static bool loom_canonicalize_memory_access_has_empty_footprint(
    const loom_module_t* module, const loom_op_t* op,
    loom_memory_access_t access) {
  if (!loom_canonicalize_memory_access_optional_roles_are_empty(module,
                                                                access)) {
    return false;
  }

  switch (loom_memory_access_operation_kind(access)) {
    case LOOM_MEMORY_ACCESS_OPERATION_LOAD:
    case LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_LOAD:
      return loom_canonicalize_memory_access_results_are_empty(module, op);
    case LOOM_MEMORY_ACCESS_OPERATION_STORE:
    case LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_STORE:
    case LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_REDUCE:
      return loom_canonicalize_required_empty_value(
          module, loom_memory_access_value(access));
    case LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_RMW:
      return loom_canonicalize_memory_access_results_are_empty(module, op) &&
             loom_canonicalize_required_empty_value(
                 module, loom_memory_access_value(access));
    case LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_CMPXCHG:
      return loom_canonicalize_memory_access_results_are_empty(module, op) &&
             loom_canonicalize_required_empty_value(
                 module, loom_memory_access_expected(access)) &&
             loom_canonicalize_required_empty_value(
                 module, loom_memory_access_replacement(access));
    case LOOM_MEMORY_ACCESS_OPERATION_PREFETCH:
    case LOOM_MEMORY_ACCESS_OPERATION_COUNT_:
      return false;
  }
  return false;
}

static iree_status_t loom_canonicalize_try_elide_empty_memory_effect(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_elided) {
  *out_elided = false;

  loom_memory_access_t access = loom_memory_access_cast(rewriter->module, op);
  if (!loom_memory_access_isa(access)) {
    return iree_ok_status();
  }
  if (!loom_canonicalize_memory_access_has_empty_footprint(rewriter->module, op,
                                                           access)) {
    return iree_ok_status();
  }

  if (op->result_count != 0) {
    IREE_RETURN_IF_ERROR(
        loom_rewriter_replace_results_with_materialized_values_and_erase(
            rewriter, op, loom_empty_build));
    *out_elided = true;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_rewriter_erase(rewriter, op));
  *out_elided = true;
  return iree_ok_status();
}

static iree_status_t loom_canonicalize_try_elide_empty_vector_op(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_elided) {
  *out_elided = false;

  IREE_RETURN_IF_ERROR(loom_canonicalize_try_replace_empty_extract_with_poison(
      rewriter, op, out_elided));
  if (*out_elided) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_canonicalize_try_fold_empty_accumulator_op(
      rewriter, op, out_elided));
  if (*out_elided) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_canonicalize_try_elide_empty_memory_effect(
      rewriter, op, out_elided));
  if (*out_elided) {
    return iree_ok_status();
  }

  if (!loom_canonicalize_can_replace_results_with_empty(rewriter->module, op)) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_results_with_materialized_values_and_erase(
          rewriter, op, loom_empty_build));
  *out_elided = true;
  return iree_ok_status();
}

static iree_status_t loom_canonicalize_try_symbolic_index_sub(
    loom_rewriter_t* rewriter, loom_symbolic_expr_context_t* expression_context,
    loom_op_t* op, bool* out_changed) {
  *out_changed = false;
  if (!loom_index_sub_isa(op)) {
    return iree_ok_status();
  }

  loom_symbolic_value_difference_t difference = {0};
  IREE_RETURN_IF_ERROR(loom_symbolic_expr_simplify_value_difference(
      expression_context, loom_index_sub_lhs(op), loom_index_sub_rhs(op),
      &difference));
  switch (difference.kind) {
    case LOOM_SYMBOLIC_VALUE_DIFFERENCE_CONSTANT: {
      IREE_RETURN_IF_ERROR(
          loom_canonicalize_replace_single_result_with_exact_i64(
              rewriter, op, difference.constant));
      *out_changed = true;
      return iree_ok_status();
    }
    case LOOM_SYMBOLIC_VALUE_DIFFERENCE_VALUE: {
      loom_type_t result_type = loom_module_value_type(
          rewriter->module, loom_op_const_results(op)[0]);
      loom_type_t replacement_type =
          loom_module_value_type(rewriter->module, difference.value_id);
      if (!loom_type_equal(result_type, replacement_type)) {
        return iree_ok_status();
      }
      IREE_RETURN_IF_ERROR(loom_canonicalize_replace_single_result_with_value(
          rewriter, op, difference.value_id));
      *out_changed = true;
      return iree_ok_status();
    }
    case LOOM_SYMBOLIC_VALUE_DIFFERENCE_UNKNOWN:
    default:
      return iree_ok_status();
  }
}

static iree_status_t loom_canonicalize_try_symbolic_integer_cmp(
    loom_rewriter_t* rewriter, loom_symbolic_expr_context_t* expression_context,
    loom_op_t* op, bool* out_changed) {
  *out_changed = false;
  if (!loom_index_cmp_isa(op) && !loom_scalar_cmpi_isa(op)) {
    return iree_ok_status();
  }
  if (loom_index_cmp_isa(op)) {
    loom_value_id_t lhs = loom_index_cmp_lhs(op);
    loom_value_id_t rhs = loom_index_cmp_rhs(op);
    loom_type_t operand_type = loom_module_value_type(rewriter->module, lhs);
    if (!loom_type_is_scalar(operand_type)) {
      return iree_ok_status();
    }
    loom_value_facts_t lhs_facts = loom_rewriter_value_facts(rewriter, lhs);
    loom_value_facts_t rhs_facts = loom_rewriter_value_facts(rewriter, rhs);
    const loom_fact_context_t* fact_context =
        rewriter->fact_table ? &rewriter->fact_table->context : NULL;
    if (!loom_index_cmp_facts_fit_target_carrier(
            fact_context, loom_type_element_type(operand_type),
            loom_index_cmp_predicate(op), &lhs_facts, &rhs_facts)) {
      return iree_ok_status();
    }
  }

  loom_condition_integer_relation_t relation_storage[1];
  loom_condition_fact_set_t condition_facts;
  loom_condition_fact_set_initialize(
      relation_storage, IREE_ARRAYSIZE(relation_storage), &condition_facts);
  bool complete = false;
  IREE_RETURN_IF_ERROR(loom_condition_facts_query(
      &expression_context->condition_query, rewriter->fact_table,
      loom_op_const_results(op)[0], /*assumed_truth=*/true, &condition_facts,
      &complete));
  if (!complete) {
    return iree_ok_status();
  }
  if (condition_facts.integer_relation_count != 1) {
    return iree_ok_status();
  }
  const loom_condition_integer_relation_t* relation =
      &condition_facts.integer_relations[0];
  if (relation->left.kind != LOOM_CONDITION_INTEGER_OPERAND_VALUE ||
      relation->right.kind != LOOM_CONDITION_INTEGER_OPERAND_VALUE) {
    return iree_ok_status();
  }

  loom_symbolic_proof_result_t proof = LOOM_SYMBOLIC_PROOF_UNKNOWN;
  if (loom_scalar_cmpi_isa(op)) {
    // Scalar comparisons describe data-path predicates. Use facts already
    // established by surrounding control flow instead of speculating through
    // every select in an unrolled data graph.
    IREE_RETURN_IF_ERROR(
        loom_symbolic_expr_prove_value_relation_with_active_facts(
            expression_context, relation->relation, relation->left.value_id,
            relation->right.value_id, &proof));
  } else {
    IREE_RETURN_IF_ERROR(loom_symbolic_expr_prove_value_relation(
        expression_context, relation->relation, relation->left.value_id,
        relation->right.value_id, &proof));
  }
  if (proof == LOOM_SYMBOLIC_PROOF_UNKNOWN) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_canonicalize_replace_single_result_with_exact_i64(
      rewriter, op, proof == LOOM_SYMBOLIC_PROOF_TRUE ? 1 : 0));
  *out_changed = true;
  return iree_ok_status();
}

static iree_status_t loom_canonicalize_try_symbolic_integer_cleanup(
    loom_rewriter_t* rewriter, loom_symbolic_expr_context_t* expression_context,
    loom_op_t* op, bool* out_changed) {
  *out_changed = false;

  IREE_RETURN_IF_ERROR(loom_canonicalize_try_symbolic_index_sub(
      rewriter, expression_context, op, out_changed));
  if (*out_changed) {
    return iree_ok_status();
  }

  return loom_canonicalize_try_symbolic_integer_cmp(
      rewriter, expression_context, op, out_changed);
}

struct loom_canonicalizer_state_t {
  // Shared greedy rewrite session used by canonicalize region runs.
  loom_greedy_rewrite_driver_t rewrite_driver;
};

static void loom_canonicalizer_reset_run_state(
    loom_canonicalizer_t* canonicalizer) {
  if (canonicalizer->state) {
    loom_greedy_rewrite_driver_reset(&canonicalizer->state->rewrite_driver);
    loom_greedy_rewrite_driver_set_fact_table(
        &canonicalizer->state->rewrite_driver, NULL);
  } else if (canonicalizer->scratch_arena_initialized) {
    iree_arena_reset(&canonicalizer->scratch_arena);
  }
}

static void loom_canonicalizer_merge_result(
    loom_canonicalizer_result_t* target,
    const loom_canonicalizer_result_t* source) {
  if (!target || !source) {
    return;
  }
  target->changed |= source->changed;
  target->facts_changed |= source->facts_changed;
  target->types_changed |= source->types_changed;
  target->boundary_maybe_changed |= source->boundary_maybe_changed;
  target->ops_modified += source->ops_modified;
  target->type_propagation_conflicts += source->type_propagation_conflicts;
  target->type_propagation_rejection_cache_hits +=
      source->type_propagation_rejection_cache_hits;
}

iree_status_t loom_canonicalizer_initialize(
    loom_module_t* module, iree_arena_allocator_t* parent_arena,
    loom_pass_value_fact_owner_t* value_facts,
    loom_canonicalizer_t* out_canonicalizer) {
  memset(out_canonicalizer, 0, sizeof(*out_canonicalizer));
  loom_canonicalizer_state_t* state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(parent_arena, sizeof(*state), (void**)&state));
  memset(state, 0, sizeof(*state));
  out_canonicalizer->module = module;
  out_canonicalizer->value_facts = value_facts;
  out_canonicalizer->parent_arena = parent_arena;
  out_canonicalizer->state = state;
  iree_arena_initialize(parent_arena->block_pool,
                        &out_canonicalizer->scratch_arena);
  out_canonicalizer->scratch_arena_initialized = true;
  loom_greedy_rewrite_driver_initialize(
      module, &out_canonicalizer->scratch_arena,
      /*fact_table=*/NULL, &state->rewrite_driver);
  return iree_ok_status();
}

void loom_canonicalizer_deinitialize(loom_canonicalizer_t* canonicalizer) {
  if (!canonicalizer) {
    return;
  }
  loom_canonicalizer_reset_run_state(canonicalizer);
  if (canonicalizer->scratch_arena_initialized) {
    iree_arena_deinitialize(&canonicalizer->scratch_arena);
  }
  memset(canonicalizer, 0, sizeof(*canonicalizer));
}

const loom_value_fact_table_t* loom_canonicalizer_fact_table(
    const loom_canonicalizer_t* canonicalizer) {
  if (!canonicalizer || !canonicalizer->state) {
    return NULL;
  }
  return loom_greedy_rewrite_driver_fact_table(
      &canonicalizer->state->rewrite_driver);
}

typedef struct loom_canonicalize_rewrite_state_t {
  // Symbolic expression context for exact address/integer cleanup.
  loom_symbolic_expr_context_t expression_context;

  // Table-driven type propagator for this region run.
  loom_type_propagator_t* type_propagator;

  // Cumulative type propagation activity captured before region cleanup.
  loom_type_propagator_statistics_t type_propagator_statistics;

  // Borrowed whole-module owner permitting callable boundary type changes.
  loom_type_propagator_boundary_callback_t refine_boundary;

  // Representation-changing rewrites enabled for this run.
  loom_canonicalizer_flags_t flags;

  // True after expression_context has been initialized.
  bool expression_context_initialized;
} loom_canonicalize_rewrite_state_t;

static iree_status_t loom_canonicalize_prepare_region(
    void* user_data, loom_greedy_rewrite_driver_t* driver,
    loom_func_like_t function, loom_region_t* region, loom_op_t* parent_op) {
  loom_canonicalize_rewrite_state_t* state =
      (loom_canonicalize_rewrite_state_t*)user_data;
  IREE_RETURN_IF_ERROR(loom_type_propagator_allocate(
      driver->module, state->refine_boundary, driver->scratch_arena,
      &state->type_propagator));
  IREE_RETURN_IF_ERROR(loom_type_propagator_prepare_region(
      state->type_propagator, region, parent_op));
  loom_symbolic_expr_context_initialize(
      driver->module, loom_type_propagator_value_domain(state->type_propagator),
      driver->rewriter.fact_table, driver->scratch_arena,
      &state->expression_context);
  state->expression_context_initialized = true;
  return iree_ok_status();
}

static void loom_canonicalize_cleanup_region(
    void* user_data, loom_greedy_rewrite_driver_t* driver) {
  loom_canonicalize_rewrite_state_t* state =
      (loom_canonicalize_rewrite_state_t*)user_data;
  state->type_propagator_statistics =
      loom_type_propagator_statistics(state->type_propagator);
  loom_type_propagator_deinitialize(state->type_propagator);
  state->type_propagator = NULL;
  state->expression_context_initialized = false;
}

static void loom_canonicalize_reset_symbolic_context(
    void* user_data, loom_greedy_rewrite_driver_t* driver) {
  loom_canonicalize_rewrite_state_t* state =
      (loom_canonicalize_rewrite_state_t*)user_data;
  if (state->expression_context_initialized) {
    loom_symbolic_expr_context_reset(&state->expression_context);
  }
}

static iree_status_t loom_canonicalize_before_worklist(
    void* user_data, loom_greedy_rewrite_driver_t* driver,
    loom_region_t* region, loom_greedy_rewrite_result_t* result,
    bool* out_changed) {
  loom_canonicalize_rewrite_state_t* state =
      (loom_canonicalize_rewrite_state_t*)user_data;
  loom_type_propagator_begin_iteration(state->type_propagator);
  driver->rewriter.flags = 0;
  return loom_branch_facts_materialize_region(
      &driver->rewriter, &state->expression_context.condition_query, region,
      result, out_changed);
}

static iree_status_t loom_canonicalize_rewrite_op(
    void* user_data, loom_greedy_rewrite_driver_t* driver, loom_op_t* op,
    loom_greedy_rewrite_result_t* result, bool* out_changed) {
  *out_changed = false;
  loom_canonicalize_rewrite_state_t* state =
      (loom_canonicalize_rewrite_state_t*)user_data;
  loom_rewriter_t* rewriter = &driver->rewriter;

  // Mini-DCE: erase trivially dead ops before fold/canonicalize.
  bool erased = false;
  rewriter->flags = 0;
  IREE_RETURN_IF_ERROR(loom_rewriter_erase_if_dead(rewriter, op, &erased));
  if (erased) {
    loom_greedy_rewrite_result_record_change(
        result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_NONE);
    *out_changed = true;
    return iree_ok_status();
  }

  // Static empty vectors have a valid empty aggregate value, not poison.
  // Handle them before poison propagation so zero-lane computations and
  // zero-footprint memory effects disappear without observing operands.
  bool empty_elided = false;
  rewriter->flags = 0;
  IREE_RETURN_IF_ERROR(
      loom_canonicalize_try_elide_empty_vector_op(rewriter, op, &empty_elided));
  if (empty_elided) {
    loom_greedy_rewrite_result_record_change(
        result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
    *out_changed = true;
    return iree_ok_status();
  }

  // Poison propagation: pure scalar/vector-result ops with any poison
  // operand become typed poison values. Boundary diagnostics decide later
  // whether the remaining poison is observable.
  bool poison_propagated = false;
  rewriter->flags = 0;
  IREE_RETURN_IF_ERROR(
      loom_canonicalize_try_propagate_poison(rewriter, op, &poison_propagated));
  if (poison_propagated) {
    loom_greedy_rewrite_result_record_change(
        result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
    *out_changed = true;
    return iree_ok_status();
  }

  // Try fold: constant fold via facts and replace with constants.
  bool folded = false;
  rewriter->flags = 0;
  IREE_RETURN_IF_ERROR(loom_rewriter_try_fold(rewriter, op, &folded));
  if (folded) {
    loom_greedy_rewrite_result_record_change(
        result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
    *out_changed = true;
    return iree_ok_status();
  }
  loom_greedy_rewrite_result_record_rewriter_flags(result, rewriter);

  const loom_op_vtable_t* vtable = loom_op_vtable(driver->module, op);

  // Table-driven type propagation: generated equality constraints and
  // value facts can narrow dynamic shapes, encoding roles, and static
  // attachments without hand-writing one pattern per op family.
  bool types_propagated = false;
  if (loom_type_propagator_may_apply_op(state->type_propagator, rewriter, op,
                                        vtable)) {
    rewriter->flags = 0;
    IREE_RETURN_IF_ERROR(loom_type_propagator_apply_op(
        state->type_propagator, rewriter, op, &types_propagated));
    if (types_propagated) {
      loom_greedy_rewrite_result_record_change(
          result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
      *out_changed = true;
      return iree_ok_status();
    }
  }

  // Symbolic address-domain cleanup uses the generic expression analysis
  // for exact linear cancellation and relation proofs that are awkward as
  // op-local patterns.
  bool symbolic_changed = false;
  rewriter->flags = 0;
  IREE_RETURN_IF_ERROR(loom_canonicalize_try_symbolic_integer_cleanup(
      rewriter, &state->expression_context, op, &symbolic_changed));
  if (symbolic_changed) {
    loom_greedy_rewrite_result_record_change(
        result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
    *out_changed = true;
    return iree_ok_status();
  }

  if (loom_view_load_isa(op) &&
      iree_any_bit_set(state->flags,
                       LOOM_CANONICALIZER_FLAG_COALESCE_VIEW_LOADS)) {
    bool combined = false;
    rewriter->flags = 0;
    IREE_RETURN_IF_ERROR(loom_view_load_coalescing_rewrite(
        rewriter, &state->expression_context, op, &combined));
    if (combined) {
      loom_greedy_rewrite_result_record_change(
          result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
      *out_changed = true;
      return iree_ok_status();
    }
  }

  // Structural canonicalization patterns.
  if (vtable && vtable->canonicalize) {
    rewriter->flags = 0;
    IREE_RETURN_IF_ERROR(vtable->canonicalize(op, rewriter));
    if (iree_any_bit_set(rewriter->flags, LOOM_REWRITER_FLAG_CHANGED)) {
      loom_greedy_rewrite_result_record_change(
          result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
      *out_changed = true;
      return iree_ok_status();
    }
  }
  if ((loom_vector_from_elements_isa(op) || loom_vector_table_lookup_isa(op)) &&
      iree_any_bit_set(state->flags,
                       LOOM_CANONICALIZER_FLAG_COMBINE_TABLE_LOOKUPS)) {
    bool combined = false;
    if (loom_vector_from_elements_isa(op)) {
      IREE_RETURN_IF_ERROR(
          loom_vector_from_elements_to_table_lookup(op, rewriter, &combined));
    } else {
      IREE_RETURN_IF_ERROR(
          loom_vector_table_lookup_simplify_indices(op, rewriter, &combined));
    }
    if (combined) {
      loom_greedy_rewrite_result_record_change(
          result, rewriter, LOOM_GREEDY_REWRITE_CHANGE_FLAG_COUNT_MODIFIED_OP);
      *out_changed = true;
      return iree_ok_status();
    }
  }
  loom_greedy_rewrite_result_record_rewriter_flags(result, rewriter);
  return iree_ok_status();
}

static void loom_canonicalizer_import_greedy_result(
    const loom_greedy_rewrite_result_t* source,
    loom_canonicalizer_result_t* target) {
  if (!source || !target) {
    return;
  }
  *target = (loom_canonicalizer_result_t){
      .changed = source->changed,
      .facts_changed = source->facts_changed,
      .types_changed = source->types_changed,
      .boundary_maybe_changed = source->boundary_maybe_changed,
      .ops_modified = source->ops_modified,
  };
}

static iree_status_t loom_canonicalizer_run_precomputed_region(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    loom_region_t* region, loom_op_t* parent_op,
    const loom_canonicalizer_options_t* options,
    loom_canonicalizer_result_t* out_result) {
  if (out_result) {
    memset(out_result, 0, sizeof(*out_result));
  }
  loom_canonicalize_rewrite_state_t state = {
      .flags = options ? options->flags : 0,
      .refine_boundary = options
                             ? options->refine_boundary
                             : (loom_type_propagator_boundary_callback_t){0},
  };
  uint32_t max_iterations = options && options->max_iterations > 0
                                ? options->max_iterations
                                : LOOM_CANONICALIZER_DEFAULT_MAX_ITERATIONS;
  loom_greedy_rewrite_options_t rewrite_options = {
      .max_iterations = max_iterations,
      .materialize_constant = loom_constant_build,
      .math_policy = options ? options->math_policy : NULL,
  };
  loom_greedy_rewrite_callbacks_t callbacks = {
      .user_data = &state,
      .prepare_region = loom_canonicalize_prepare_region,
      .cleanup_region = loom_canonicalize_cleanup_region,
      .before_worklist = loom_canonicalize_before_worklist,
      .rewrite_op = loom_canonicalize_rewrite_op,
      .changed = loom_canonicalize_reset_symbolic_context,
  };
  loom_greedy_rewrite_result_t rewrite_result = {0};
  iree_status_t status = loom_greedy_rewrite_run_region(
      &canonicalizer->state->rewrite_driver, function, region, parent_op,
      &rewrite_options, &callbacks, &rewrite_result);
  if (iree_status_is_ok(status)) {
    loom_canonicalizer_import_greedy_result(&rewrite_result, out_result);
    if (out_result) {
      out_result->type_propagation_conflicts =
          (int64_t)state.type_propagator_statistics.conflict_count;
      out_result->type_propagation_rejection_cache_hits =
          (int64_t)state.type_propagator_statistics.rejection_cache_hit_count;
    }
  }
  return status;
}

static iree_status_t loom_canonicalizer_prepare_region_facts(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    loom_region_t* region, loom_op_t* parent_op,
    const loom_canonicalizer_options_t* options) {
  loom_value_fact_table_t* facts = NULL;
  IREE_RETURN_IF_ERROR(loom_pass_value_fact_owner_prepare(
      canonicalizer->value_facts, canonicalizer->module,
      loom_pass_value_fact_scope_region_for_target(
          function, region, parent_op, options ? options->target_facts : NULL),
      &facts));
  if (options && options->seed_facts.table) {
    IREE_RETURN_IF_ERROR(loom_value_fact_table_clone_values(
        facts, options->seed_facts, canonicalizer->module));
  }
  IREE_RETURN_IF_ERROR(loom_value_fact_table_compute_region(
      facts, canonicalizer->module, function, region, parent_op));
  loom_greedy_rewrite_driver_set_fact_table(
      &canonicalizer->state->rewrite_driver, facts);
  return iree_ok_status();
}

static iree_status_t loom_canonicalizer_prepare_function_facts(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    const loom_canonicalizer_options_t* options) {
  const loom_pass_value_fact_scope_t scope =
      loom_pass_value_fact_scope_function_for_target(
          function, options ? options->target_facts : NULL);
  loom_value_fact_table_t* facts = NULL;
  if (options && options->seed_facts.table) {
    IREE_RETURN_IF_ERROR(loom_pass_value_fact_owner_prepare(
        canonicalizer->value_facts, canonicalizer->module, scope, &facts));
    IREE_RETURN_IF_ERROR(loom_value_fact_table_clone_values(
        facts, options->seed_facts, canonicalizer->module));
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_compute(facts, canonicalizer->module, function));
  } else {
    IREE_RETURN_IF_ERROR(loom_pass_value_fact_owner_acquire(
        canonicalizer->value_facts, canonicalizer->module, scope, &facts));
  }
  loom_greedy_rewrite_driver_set_fact_table(
      &canonicalizer->state->rewrite_driver, facts);
  return iree_ok_status();
}

iree_status_t loom_canonicalizer_run_region(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    loom_region_t* region, loom_op_t* parent_op,
    const loom_canonicalizer_options_t* options,
    loom_canonicalizer_result_t* out_result) {
  if (out_result) {
    memset(out_result, 0, sizeof(*out_result));
  }
  loom_canonicalizer_reset_run_state(canonicalizer);
  if (!region) {
    return iree_ok_status();
  }
  iree_status_t status = loom_canonicalizer_prepare_region_facts(
      canonicalizer, function, region, parent_op, options);
  if (iree_status_is_ok(status)) {
    status = loom_canonicalizer_run_precomputed_region(
        canonicalizer, function, region, parent_op, options, out_result);
  }
  if (!iree_status_is_ok(status)) {
    loom_pass_value_fact_owner_invalidate(canonicalizer->value_facts);
    loom_greedy_rewrite_driver_set_fact_table(
        &canonicalizer->state->rewrite_driver, NULL);
  }
  return status;
}

iree_status_t loom_canonicalizer_run_function(
    loom_canonicalizer_t* canonicalizer, loom_func_like_t function,
    const loom_canonicalizer_options_t* options,
    loom_canonicalizer_result_t* out_result) {
  if (out_result) {
    memset(out_result, 0, sizeof(*out_result));
  }
  loom_canonicalizer_reset_run_state(canonicalizer);
  loom_region_t* body = loom_func_like_body(function);
  if (!body) {
    return iree_ok_status();
  }

  iree_status_t status = loom_canonicalizer_prepare_function_facts(
      canonicalizer, function, options);
  if (!iree_status_is_ok(status)) {
    loom_pass_value_fact_owner_invalidate(canonicalizer->value_facts);
    loom_greedy_rewrite_driver_set_fact_table(
        &canonicalizer->state->rewrite_driver, NULL);
    return status;
  }

  const uint8_t body_region_index = loom_func_like_body_region_index(function);
  loom_canonicalizer_result_t aggregate_result = {0};
  for (uint8_t i = 0; i < loom_func_like_region_count(function); ++i) {
    if (i == body_region_index) {
      continue;
    }
    loom_region_t* region = loom_func_like_region(function, i);
    if (!region) {
      continue;
    }
    loom_canonicalizer_result_t region_result = {0};
    status = loom_canonicalizer_run_precomputed_region(
        canonicalizer, function, region, function.op, options, &region_result);
    if (!iree_status_is_ok(status)) {
      break;
    }
    loom_canonicalizer_merge_result(&aggregate_result, &region_result);
  }

  if (iree_status_is_ok(status)) {
    loom_canonicalizer_result_t body_result = {0};
    status = loom_canonicalizer_run_precomputed_region(
        canonicalizer, function, body, function.op, options, &body_result);
    if (iree_status_is_ok(status)) {
      loom_canonicalizer_merge_result(&aggregate_result, &body_result);
    }
  }

  if (!iree_status_is_ok(status)) {
    loom_pass_value_fact_owner_invalidate(canonicalizer->value_facts);
    loom_greedy_rewrite_driver_set_fact_table(
        &canonicalizer->state->rewrite_driver, NULL);
  }
  if (out_result) {
    *out_result = aggregate_result;
  }
  return status;
}
