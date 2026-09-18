// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/cfg_condition_facts.h"
#include "loom/analysis/cfg_condition_operand_domain.h"

const loom_cfg_condition_relation_view_t*
loom_cfg_condition_relation_table_block(
    const loom_cfg_condition_relation_table_t* table, uint16_t block_index) {
  return block_index < table->block_count ? &table->views[block_index] : NULL;
}

const loom_cfg_condition_relation_view_t*
loom_cfg_condition_relation_table_edge(
    const loom_cfg_condition_relation_table_t* table,
    loom_cfg_edge_index_t edge_index) {
  if (edge_index >= table->edge_count) {
    return NULL;
  }
  const uint32_t view = table->edge_view_indices[edge_index];
  return view < table->view_count ? &table->views[view] : NULL;
}

bool loom_cfg_condition_relation_view_query_boolean(
    const loom_cfg_condition_relation_table_t* table,
    const loom_cfg_condition_relation_view_t* view, loom_value_id_t value_id,
    bool* out_value) {
  const loom_cfg_condition_operand_t operand =
      loom_cfg_condition_operand_domain_value(table->operand_domain, value_id);
  if (operand == LOOM_CFG_CONDITION_OPERAND_INVALID) {
    return false;
  }
  const bool known_false = loom_condition_relation_set_index_contains(
      &table->set_index, view->boolean_values[0], operand);
  const bool known_true = loom_condition_relation_set_index_contains(
      &table->set_index, view->boolean_values[1], operand);
  if (known_false == known_true) {
    return false;
  }
  *out_value = known_true;
  return true;
}

loom_condition_relation_outcome_bits_t
loom_cfg_condition_relation_view_query_excluded_outcomes(
    const loom_cfg_condition_relation_table_t* table,
    const loom_cfg_condition_relation_view_t* view,
    const loom_value_fact_table_t* fact_table,
    loom_condition_integer_operand_t left,
    loom_condition_integer_operand_t right) {
  loom_cfg_condition_operand_t left_variants[2];
  loom_cfg_condition_operand_t right_variants[2];
  loom_cfg_condition_operand_domain_variants(table->operand_domain, fact_table,
                                             left, left_variants);
  loom_cfg_condition_operand_domain_variants(table->operand_domain, fact_table,
                                             right, right_variants);
  loom_condition_relation_outcome_bits_t exclusions = 0;
  for (uint8_t left_index = 0; left_index < 2; ++left_index) {
    if (left_variants[left_index] == LOOM_CFG_CONDITION_OPERAND_INVALID) {
      continue;
    }
    const loom_condition_relation_set_id_t* row =
        loom_condition_relation_matrix_view_find(&view->integer_relations,
                                                 left_variants[left_index]);
    if (row == NULL) {
      continue;
    }
    for (uint8_t right_index = 0; right_index < 2; ++right_index) {
      if (right_variants[right_index] == LOOM_CFG_CONDITION_OPERAND_INVALID) {
        continue;
      }
      for (loom_condition_relation_outcome_t outcome = 0;
           outcome < LOOM_CONDITION_RELATION_OUTCOME_COUNT; ++outcome) {
        if (loom_condition_relation_set_index_contains(
                &table->set_index, row[outcome], right_variants[right_index])) {
          exclusions |= (loom_condition_relation_outcome_bits_t)(1u << outcome);
        }
      }
    }
  }
  return exclusions;
}

static bool loom_cfg_condition_relation_from_exclusions(
    loom_condition_relation_outcome_bits_t exclusions,
    loom_symbolic_integer_relation_t* out_relation) {
  const loom_condition_relation_outcome_bits_t outcomes =
      LOOM_CONDITION_RELATION_OUTCOME_BIT_ALL &
      (loom_condition_relation_outcome_bits_t)~exclusions;
  switch (outcomes) {
    case LOOM_CONDITION_RELATION_OUTCOME_BIT_EQUAL:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
      return true;
    case LOOM_CONDITION_RELATION_OUTCOME_BIT_LESS |
        LOOM_CONDITION_RELATION_OUTCOME_BIT_GREATER:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_NE;
      return true;
    case LOOM_CONDITION_RELATION_OUTCOME_BIT_LESS:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_LT;
      return true;
    case LOOM_CONDITION_RELATION_OUTCOME_BIT_LESS |
        LOOM_CONDITION_RELATION_OUTCOME_BIT_EQUAL:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_LE;
      return true;
    case LOOM_CONDITION_RELATION_OUTCOME_BIT_GREATER:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_GT;
      return true;
    case LOOM_CONDITION_RELATION_OUTCOME_BIT_EQUAL |
        LOOM_CONDITION_RELATION_OUTCOME_BIT_GREATER:
      *out_relation = LOOM_SYMBOLIC_INTEGER_RELATION_GE;
      return true;
    default:
      return false;
  }
}

typedef struct loom_cfg_condition_relation_visit_state_t {
  // Table owning the immutable set index and operand domain.
  const loom_cfg_condition_relation_table_t* table;

  // Three excluded-outcome roots for the anchored row.
  const loom_condition_relation_set_id_t* excluded;

  // Caller-provided anchor preserved across exact-value variants.
  loom_condition_integer_operand_t anchor;

  // Caller visitor.
  loom_cfg_condition_relation_visit_fn_t visit;

  // Caller visitor state.
  void* user_data;

  // Outcome root currently being enumerated.
  loom_condition_relation_outcome_t outcome;
} loom_cfg_condition_relation_visit_state_t;

static bool loom_cfg_condition_relation_visit_member(void* user_data,
                                                     uint32_t right) {
  loom_cfg_condition_relation_visit_state_t* state =
      (loom_cfg_condition_relation_visit_state_t*)user_data;
  for (loom_condition_relation_outcome_t outcome = 0; outcome < state->outcome;
       ++outcome) {
    if (loom_condition_relation_set_index_contains(
            &state->table->set_index, state->excluded[outcome], right)) {
      return true;
    }
  }

  loom_condition_relation_outcome_bits_t exclusions = 0;
  for (loom_condition_relation_outcome_t outcome = 0;
       outcome < LOOM_CONDITION_RELATION_OUTCOME_COUNT; ++outcome) {
    if (loom_condition_relation_set_index_contains(
            &state->table->set_index, state->excluded[outcome], right)) {
      exclusions |= (loom_condition_relation_outcome_bits_t)(1u << outcome);
    }
  }
  loom_symbolic_integer_relation_t relation = 0;
  if (!loom_cfg_condition_relation_from_exclusions(exclusions, &relation)) {
    return true;
  }
  const loom_condition_integer_relation_t retained_relation = {
      .relation = relation,
      .left = state->anchor,
      .right = loom_cfg_condition_operand_domain_expand(
          state->table->operand_domain, right),
  };
  return state->visit(state->user_data, &retained_relation);
}

bool loom_cfg_condition_relation_view_for_each_while(
    const loom_cfg_condition_relation_table_t* table,
    const loom_cfg_condition_relation_view_t* view,
    const loom_value_fact_table_t* fact_table,
    loom_condition_integer_operand_t anchor,
    loom_cfg_condition_relation_visit_fn_t visit, void* user_data) {
  loom_cfg_condition_operand_t anchor_variants[2];
  loom_cfg_condition_operand_domain_variants(table->operand_domain, fact_table,
                                             anchor, anchor_variants);
  for (uint8_t anchor_index = 0; anchor_index < 2; ++anchor_index) {
    if (anchor_variants[anchor_index] == LOOM_CFG_CONDITION_OPERAND_INVALID) {
      continue;
    }
    const loom_condition_relation_set_id_t* excluded =
        loom_condition_relation_matrix_view_find(&view->integer_relations,
                                                 anchor_variants[anchor_index]);
    if (excluded == NULL) {
      continue;
    }
    loom_cfg_condition_relation_visit_state_t state = {
        .table = table,
        .excluded = excluded,
        .anchor = anchor,
        .visit = visit,
        .user_data = user_data,
    };
    for (loom_condition_relation_outcome_t outcome = 0;
         outcome < LOOM_CONDITION_RELATION_OUTCOME_COUNT; ++outcome) {
      state.outcome = outcome;
      if (!loom_condition_relation_set_index_for_each_while(
              &table->set_index, excluded[outcome],
              loom_cfg_condition_relation_visit_member, &state)) {
        return false;
      }
    }
  }
  return true;
}
