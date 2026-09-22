// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/liveness_dataflow.h"

#include <string.h>

typedef struct loom_liveness_dataflow_block_state_t {
  // Mutable exact live-in value list.
  loom_value_id_t* live_in_values;
  // Number of live-in relations counted for this block.
  iree_host_size_t live_in_count;
  // Number of live-in values published for this block.
  iree_host_size_t live_in_fill_count;
  // Mutable exact live-out value list.
  loom_value_id_t* live_out_values;
  // Number of live-out relations counted for this block.
  iree_host_size_t live_out_count;
  // Number of live-out values published for this block.
  iree_host_size_t live_out_fill_count;
} loom_liveness_dataflow_block_state_t;

typedef enum loom_liveness_dataflow_pass_e {
  // Count exact relations before allocating published value lists.
  LOOM_LIVENESS_DATAFLOW_PASS_COUNT = 0,
  // Fill the exact-size published value lists.
  LOOM_LIVENESS_DATAFLOW_PASS_PUBLISH = 1,
} loom_liveness_dataflow_pass_t;

typedef struct loom_liveness_dataflow_solver_t {
  // CFG predecessor index, or NULL for a local non-CFG region.
  const loom_cfg_graph_t* cfg_graph;
  // Value IDs indexed by region-local ordinal.
  const loom_value_id_t* value_ids;
  // Number of entries in |value_ids|.
  loom_value_ordinal_t value_count;
  // Block transfer facts in region order.
  const loom_liveness_block_transfer_t* block_transfers;
  // Number of entries in |block_transfers|.
  uint16_t block_count;
  // Mutable relation counts and published lists in region order.
  loom_liveness_dataflow_block_state_t* block_states;
  // Prefix offsets into |use_block_indices| for each value ordinal.
  iree_host_size_t* value_use_offsets;
  // Blocks with an upward-exposed use, grouped by value ordinal.
  uint16_t* use_block_indices;
  // Defining block index for each value ordinal, or UINT32_MAX.
  uint32_t* definition_block_indices;
  // Value ordinal most recently found live-in at each block.
  loom_value_ordinal_t* live_in_marks;
  // Value ordinal most recently found live-out at each block.
  loom_value_ordinal_t* live_out_marks;
  // Blocks awaiting predecessor propagation for the current value.
  uint16_t* block_worklist;
} loom_liveness_dataflow_solver_t;

static iree_status_t loom_liveness_dataflow_index_transfers(
    loom_liveness_dataflow_solver_t* solver,
    iree_arena_allocator_t* scratch_arena) {
  if (solver->value_count == 0) {
    return iree_ok_status();
  }

  const iree_host_size_t offset_count =
      (iree_host_size_t)solver->value_count + 1u;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, offset_count, sizeof(*solver->value_use_offsets),
      (void**)&solver->value_use_offsets));
  memset(solver->value_use_offsets, 0,
         offset_count * sizeof(*solver->value_use_offsets));

  iree_host_size_t total_use_count = 0;
  for (uint16_t block_index = 0; block_index < solver->block_count;
       ++block_index) {
    const loom_liveness_block_transfer_t* transfer =
        &solver->block_transfers[block_index];
    if (transfer->use_count > IREE_HOST_SIZE_MAX - total_use_count) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "liveness block uses exceed host size");
    }
    total_use_count += transfer->use_count;
    for (iree_host_size_t i = 0; i < transfer->use_count; ++i) {
      ++solver->value_use_offsets[transfer->use_ordinals[i] + 1u];
    }
  }
  for (iree_host_size_t i = 0; i < solver->value_count; ++i) {
    solver->value_use_offsets[i + 1u] += solver->value_use_offsets[i];
  }
  IREE_ASSERT_EQ(solver->value_use_offsets[solver->value_count],
                 total_use_count);

  if (total_use_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        scratch_arena, total_use_count, sizeof(*solver->use_block_indices),
        (void**)&solver->use_block_indices));
  }
  uint32_t* value_use_fill_counts = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, solver->value_count, sizeof(*value_use_fill_counts),
      (void**)&value_use_fill_counts));
  memset(value_use_fill_counts, 0,
         solver->value_count * sizeof(*value_use_fill_counts));
  for (uint16_t block_index = 0; block_index < solver->block_count;
       ++block_index) {
    const loom_liveness_block_transfer_t* transfer =
        &solver->block_transfers[block_index];
    for (iree_host_size_t i = 0; i < transfer->use_count; ++i) {
      const loom_value_ordinal_t value_ordinal = transfer->use_ordinals[i];
      const iree_host_size_t use_index =
          solver->value_use_offsets[value_ordinal] +
          value_use_fill_counts[value_ordinal]++;
      solver->use_block_indices[use_index] = block_index;
    }
  }

  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(scratch_arena, solver->value_count,
                                sizeof(*solver->definition_block_indices),
                                (void**)&solver->definition_block_indices));
  for (iree_host_size_t i = 0; i < solver->value_count; ++i) {
    solver->definition_block_indices[i] = UINT32_MAX;
  }
  for (uint16_t block_index = 0; block_index < solver->block_count;
       ++block_index) {
    const loom_liveness_block_transfer_t* transfer =
        &solver->block_transfers[block_index];
    for (iree_host_size_t i = 0; i < transfer->definition_count; ++i) {
      const loom_value_ordinal_t value_ordinal =
          transfer->definition_ordinals[i];
      IREE_ASSERT(
          solver->definition_block_indices[value_ordinal] == UINT32_MAX ||
          solver->definition_block_indices[value_ordinal] == block_index);
      solver->definition_block_indices[value_ordinal] = block_index;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_liveness_dataflow_solver_initialize(
    const loom_cfg_graph_t* cfg_graph, const loom_value_id_t* value_ids,
    loom_value_ordinal_t value_count,
    const loom_liveness_block_transfer_t* block_transfers, uint16_t block_count,
    loom_liveness_dataflow_solver_t* out_solver,
    iree_arena_allocator_t* scratch_arena) {
  loom_liveness_dataflow_solver_t solver = {
      .cfg_graph = cfg_graph,
      .value_ids = value_ids,
      .value_count = value_count,
      .block_transfers = block_transfers,
      .block_count = block_count,
  };
  if (block_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        scratch_arena, block_count, sizeof(*solver.block_states),
        (void**)&solver.block_states));
    memset(solver.block_states, 0, block_count * sizeof(*solver.block_states));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        scratch_arena, block_count, sizeof(*solver.live_in_marks),
        (void**)&solver.live_in_marks));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        scratch_arena, block_count, sizeof(*solver.live_out_marks),
        (void**)&solver.live_out_marks));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        scratch_arena, block_count, sizeof(*solver.block_worklist),
        (void**)&solver.block_worklist));
  }
  IREE_RETURN_IF_ERROR(
      loom_liveness_dataflow_index_transfers(&solver, scratch_arena));
  *out_solver = solver;
  return iree_ok_status();
}

static void loom_liveness_dataflow_reset_marks(
    loom_liveness_dataflow_solver_t* solver) {
  for (uint16_t block_index = 0; block_index < solver->block_count;
       ++block_index) {
    solver->live_in_marks[block_index] = LOOM_VALUE_ORDINAL_INVALID;
    solver->live_out_marks[block_index] = LOOM_VALUE_ORDINAL_INVALID;
  }
}

static void loom_liveness_dataflow_record_live_in(
    loom_liveness_dataflow_solver_t* solver, uint16_t block_index,
    loom_value_ordinal_t value_ordinal, loom_liveness_dataflow_pass_t pass) {
  loom_liveness_dataflow_block_state_t* block_state =
      &solver->block_states[block_index];
  if (pass == LOOM_LIVENESS_DATAFLOW_PASS_COUNT) {
    ++block_state->live_in_count;
    return;
  }
  IREE_ASSERT_LT(block_state->live_in_fill_count, block_state->live_in_count);
  block_state->live_in_values[block_state->live_in_fill_count++] =
      solver->value_ids[value_ordinal];
}

static void loom_liveness_dataflow_record_live_out(
    loom_liveness_dataflow_solver_t* solver, uint16_t block_index,
    loom_value_ordinal_t value_ordinal, loom_liveness_dataflow_pass_t pass) {
  loom_liveness_dataflow_block_state_t* block_state =
      &solver->block_states[block_index];
  if (pass == LOOM_LIVENESS_DATAFLOW_PASS_COUNT) {
    ++block_state->live_out_count;
    return;
  }
  IREE_ASSERT_LT(block_state->live_out_fill_count, block_state->live_out_count);
  block_state->live_out_values[block_state->live_out_fill_count++] =
      solver->value_ids[value_ordinal];
}

static void loom_liveness_dataflow_solver_run(
    loom_liveness_dataflow_solver_t* solver,
    loom_liveness_dataflow_pass_t pass) {
  loom_liveness_dataflow_reset_marks(solver);
  for (loom_value_ordinal_t value_ordinal = 0;
       value_ordinal < solver->value_count; ++value_ordinal) {
    iree_host_size_t worklist_count = 0;
    const iree_host_size_t use_start = solver->value_use_offsets[value_ordinal];
    const iree_host_size_t use_end =
        solver->value_use_offsets[value_ordinal + 1u];
    for (iree_host_size_t i = use_start; i < use_end; ++i) {
      const uint16_t block_index = solver->use_block_indices[i];
      if (solver->live_in_marks[block_index] == value_ordinal) {
        continue;
      }
      solver->live_in_marks[block_index] = value_ordinal;
      loom_liveness_dataflow_record_live_in(solver, block_index, value_ordinal,
                                            pass);
      solver->block_worklist[worklist_count++] = block_index;
    }

    while (worklist_count > 0) {
      const uint16_t block_index = solver->block_worklist[--worklist_count];
      if (solver->cfg_graph == NULL) {
        continue;
      }
      const loom_cfg_block_index_span_t predecessors =
          loom_cfg_graph_predecessors(solver->cfg_graph, block_index);
      for (iree_host_size_t i = 0; i < predecessors.count; ++i) {
        const uint16_t predecessor = predecessors.values[i];
        if (solver->live_out_marks[predecessor] != value_ordinal) {
          solver->live_out_marks[predecessor] = value_ordinal;
          loom_liveness_dataflow_record_live_out(solver, predecessor,
                                                 value_ordinal, pass);
        }
        if (solver->definition_block_indices[value_ordinal] == predecessor ||
            solver->live_in_marks[predecessor] == value_ordinal) {
          continue;
        }
        solver->live_in_marks[predecessor] = value_ordinal;
        loom_liveness_dataflow_record_live_in(solver, predecessor,
                                              value_ordinal, pass);
        IREE_ASSERT_LT(worklist_count, solver->block_count);
        solver->block_worklist[worklist_count++] = predecessor;
      }
    }
  }
}

static iree_status_t loom_liveness_dataflow_allocate_results(
    loom_liveness_dataflow_solver_t* solver,
    loom_liveness_block_relation_t* out_block_relations,
    iree_arena_allocator_t* result_arena) {
  iree_host_size_t total_live_in_count = 0;
  iree_host_size_t total_live_out_count = 0;
  for (uint16_t block_index = 0; block_index < solver->block_count;
       ++block_index) {
    const loom_liveness_dataflow_block_state_t* block_state =
        &solver->block_states[block_index];
    if (block_state->live_in_count > IREE_HOST_SIZE_MAX - total_live_in_count ||
        block_state->live_out_count >
            IREE_HOST_SIZE_MAX - total_live_out_count) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "liveness block relations exceed host size");
    }
    total_live_in_count += block_state->live_in_count;
    total_live_out_count += block_state->live_out_count;
  }

  loom_value_id_t* live_in_values = NULL;
  if (total_live_in_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        result_arena, total_live_in_count, sizeof(*live_in_values),
        (void**)&live_in_values));
  }
  loom_value_id_t* live_out_values = NULL;
  if (total_live_out_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        result_arena, total_live_out_count, sizeof(*live_out_values),
        (void**)&live_out_values));
  }

  iree_host_size_t live_in_offset = 0;
  iree_host_size_t live_out_offset = 0;
  for (uint16_t block_index = 0; block_index < solver->block_count;
       ++block_index) {
    loom_liveness_dataflow_block_state_t* block_state =
        &solver->block_states[block_index];
    loom_liveness_block_relation_t* relation =
        &out_block_relations[block_index];
    block_state->live_in_values =
        block_state->live_in_count > 0 ? live_in_values + live_in_offset : NULL;
    relation->live_in_values = block_state->live_in_values;
    relation->live_in_count = block_state->live_in_count;
    live_in_offset += block_state->live_in_count;
    block_state->live_out_values = block_state->live_out_count > 0
                                       ? live_out_values + live_out_offset
                                       : NULL;
    relation->live_out_values = block_state->live_out_values;
    relation->live_out_count = block_state->live_out_count;
    live_out_offset += block_state->live_out_count;
  }
  IREE_ASSERT_EQ(live_in_offset, total_live_in_count);
  IREE_ASSERT_EQ(live_out_offset, total_live_out_count);
  return iree_ok_status();
}

iree_status_t loom_liveness_dataflow_solve(
    const loom_cfg_graph_t* cfg_graph, const loom_value_id_t* value_ids,
    loom_value_ordinal_t value_count,
    const loom_liveness_block_transfer_t* block_transfers, uint16_t block_count,
    loom_liveness_block_relation_t* out_block_relations,
    iree_arena_allocator_t* result_arena,
    iree_arena_allocator_t* scratch_arena) {
  loom_liveness_dataflow_solver_t solver = {0};
  IREE_RETURN_IF_ERROR(loom_liveness_dataflow_solver_initialize(
      cfg_graph, value_ids, value_count, block_transfers, block_count, &solver,
      scratch_arena));
  loom_liveness_dataflow_solver_run(&solver, LOOM_LIVENESS_DATAFLOW_PASS_COUNT);
  IREE_RETURN_IF_ERROR(loom_liveness_dataflow_allocate_results(
      &solver, out_block_relations, result_arena));
  loom_liveness_dataflow_solver_run(&solver,
                                    LOOM_LIVENESS_DATAFLOW_PASS_PUBLISH);
  for (uint16_t block_index = 0; block_index < block_count; ++block_index) {
    const loom_liveness_dataflow_block_state_t* block_state =
        &solver.block_states[block_index];
    IREE_ASSERT_EQ(block_state->live_in_fill_count, block_state->live_in_count);
    IREE_ASSERT_EQ(block_state->live_out_fill_count,
                   block_state->live_out_count);
  }
  return iree_ok_status();
}
