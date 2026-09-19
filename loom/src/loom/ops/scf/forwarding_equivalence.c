// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/scf/forwarding_equivalence.h"

#include <string.h>

typedef struct loom_scf_forwarding_group_t {
  // First state position in the partition order.
  uint16_t start;

  // Number of states in the group.
  uint16_t count;

  // Number of predecessor states marked by the current splitter.
  uint16_t marked_count;
} loom_scf_forwarding_group_t;

static_assert(sizeof(loom_scf_forwarding_group_t) == 6,
              "forwarding group must remain compact");

static uint32_t loom_scf_forwarding_state_key(
    loom_scf_forwarding_equivalence_problem_t problem, uint16_t state,
    uint8_t word) {
  if (word == 0) {
    if (problem.successors[state] != LOOM_SCF_FORWARDING_TERMINAL) {
      return LOOM_VALUE_ID_INVALID;
    }
    return loom_value_fact_table_query_identity(problem.fact_table,
                                                problem.yielded_values[state]);
  }
  return loom_value_fact_table_query_identity(problem.fact_table,
                                              problem.initial_values[state]);
}

static uint8_t loom_scf_forwarding_state_key_byte(
    loom_scf_forwarding_equivalence_problem_t problem, uint16_t state,
    uint8_t byte) {
  const uint8_t word = byte / sizeof(uint32_t);
  const uint8_t shift = (byte % sizeof(uint32_t)) * 8;
  return (uint8_t)(loom_scf_forwarding_state_key(problem, state, word) >>
                   shift);
}

// Stably groups states by terminal/initial identity using fixed-width radix
// passes. The returned order and workspace may be either input array.
static uint16_t* loom_scf_forwarding_sort_states(
    loom_scf_forwarding_equivalence_problem_t problem, uint16_t* order,
    uint16_t* workspace, uint16_t** out_workspace) {
  uint32_t offsets[256];
  for (uint8_t byte = 0; byte < 2 * sizeof(uint32_t); ++byte) {
    memset(offsets, 0, sizeof(offsets));
    for (uint16_t i = 0; i < problem.count; ++i) {
      ++offsets[loom_scf_forwarding_state_key_byte(problem, order[i], byte)];
    }

    uint32_t next_offset = 0;
    for (uint16_t digit = 0; digit < IREE_ARRAYSIZE(offsets); ++digit) {
      const uint32_t digit_count = offsets[digit];
      offsets[digit] = next_offset;
      next_offset += digit_count;
    }
    for (uint16_t i = 0; i < problem.count; ++i) {
      const uint16_t state = order[i];
      const uint8_t digit =
          loom_scf_forwarding_state_key_byte(problem, state, byte);
      workspace[offsets[digit]++] = state;
    }

    uint16_t* temporary = order;
    order = workspace;
    workspace = temporary;
  }
  *out_workspace = workspace;
  return order;
}

static bool loom_scf_forwarding_initial_keys_equal(
    loom_scf_forwarding_equivalence_problem_t problem, uint16_t lhs,
    uint16_t rhs) {
  return loom_scf_forwarding_state_key(problem, lhs, 0) ==
             loom_scf_forwarding_state_key(problem, rhs, 0) &&
         loom_scf_forwarding_state_key(problem, lhs, 1) ==
             loom_scf_forwarding_state_key(problem, rhs, 1);
}

#if IREE_HAVE_ATTRIBUTE(minsize)
__attribute__((minsize))
#endif
IREE_ATTRIBUTE_NOINLINE iree_status_t loom_scf_forwarding_equivalence_partition(
    loom_scf_forwarding_equivalence_problem_t problem,
    iree_arena_allocator_t* scratch_arena, uint16_t* out_representatives,
    uint16_t* out_class_count) {
  *out_class_count = 0;
  if (problem.count == 0) {
    return iree_ok_status();
  }

  uint16_t* order = NULL;
  uint16_t* positions = NULL;
  uint16_t* state_groups = NULL;
  uint16_t* worklist = NULL;
  uint16_t* touched_groups = NULL;
  uint16_t* splitter_states = NULL;
  loom_scf_forwarding_group_t* groups = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, problem.count, sizeof(*order), (void**)&order));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, problem.count, sizeof(*positions), (void**)&positions));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(scratch_arena, problem.count,
                                                 sizeof(*state_groups),
                                                 (void**)&state_groups));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, problem.count, sizeof(*worklist), (void**)&worklist));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(scratch_arena, problem.count,
                                                 sizeof(*touched_groups),
                                                 (void**)&touched_groups));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(scratch_arena, problem.count,
                                                 sizeof(*splitter_states),
                                                 (void**)&splitter_states));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, problem.count, sizeof(*groups), (void**)&groups));

  for (uint16_t state = 0; state < problem.count; ++state) {
    order[state] = state;
  }
  order =
      loom_scf_forwarding_sort_states(problem, order, positions, &positions);

  uint16_t group_count = 0;
  for (uint16_t position = 0; position < problem.count; ++position) {
    const uint16_t state = order[position];
    if (position == 0 || !loom_scf_forwarding_initial_keys_equal(
                             problem, order[position - 1], state)) {
      groups[group_count] = (loom_scf_forwarding_group_t){
          .start = position,
      };
      ++group_count;
    }
    ++groups[group_count - 1].count;
    positions[state] = position;
    state_groups[state] = (uint16_t)(group_count - 1);
  }

  // Convert the successor map into inverse predecessor lists. The output array
  // temporarily owns each target's list head and becomes the final class map.
  memset(out_representatives, 0xFF,
         (iree_host_size_t)problem.count * sizeof(*out_representatives));
  for (uint16_t state = 0; state < problem.count; ++state) {
    const uint16_t target = problem.successors[state];
    problem.successors[state] = LOOM_SCF_FORWARDING_TERMINAL;
    if (target == LOOM_SCF_FORWARDING_TERMINAL) {
      continue;
    }
    problem.successors[state] = out_representatives[target];
    out_representatives[target] = state;
  }

  uint16_t work_head = 0;
  uint16_t work_tail = group_count;
  for (uint16_t group = 0; group < group_count; ++group) {
    worklist[group] = group;
  }

  while (work_head < work_tail) {
    const uint16_t splitter_index = worklist[work_head++];
    const uint16_t splitter_start = groups[splitter_index].start;
    const uint16_t splitter_count = groups[splitter_index].count;
    uint16_t touched_count = 0;

    // Marking predecessors reorders partition ranges. Snapshot the splitter so
    // that a swap in its own range cannot repeat or omit a target state.
    memcpy(splitter_states, order + splitter_start,
           (iree_host_size_t)splitter_count * sizeof(*splitter_states));
    for (uint16_t i = 0; i < splitter_count; ++i) {
      const uint16_t target = splitter_states[i];
      for (uint16_t state = out_representatives[target];
           state != LOOM_SCF_FORWARDING_TERMINAL;
           state = problem.successors[state]) {
        const uint16_t group_index = state_groups[state];
        loom_scf_forwarding_group_t* group = &groups[group_index];
        IREE_ASSERT_LT(group->marked_count, group->count);
        if (group->marked_count == 0) {
          touched_groups[touched_count++] = group_index;
        }

        const uint16_t old_position = positions[state];
        const uint16_t marked_position =
            (uint16_t)(group->start + group->marked_count);
        if (old_position != marked_position) {
          const uint16_t other = order[marked_position];
          order[old_position] = other;
          positions[other] = old_position;
          order[marked_position] = state;
          positions[state] = marked_position;
        }
        ++group->marked_count;
      }
    }

    for (uint16_t i = 0; i < touched_count; ++i) {
      const uint16_t group_index = touched_groups[i];
      loom_scf_forwarding_group_t* group = &groups[group_index];
      const uint16_t marked_count = group->marked_count;
      const uint16_t unmarked_count = (uint16_t)(group->count - marked_count);
      group->marked_count = 0;
      if (marked_count == 0 || unmarked_count == 0) {
        continue;
      }

      const bool marked_is_smaller = marked_count <= unmarked_count;
      const uint16_t smaller_start =
          marked_is_smaller ? group->start
                            : (uint16_t)(group->start + marked_count);
      const uint16_t smaller_count =
          marked_is_smaller ? marked_count : unmarked_count;
      const uint16_t larger_start =
          marked_is_smaller ? (uint16_t)(group->start + marked_count)
                            : group->start;
      const uint16_t larger_count =
          marked_is_smaller ? unmarked_count : marked_count;

      group->start = larger_start;
      group->count = larger_count;
      const uint16_t new_group_index = group_count++;
      groups[new_group_index] = (loom_scf_forwarding_group_t){
          .start = smaller_start,
          .count = smaller_count,
      };
      for (uint16_t j = 0; j < smaller_count; ++j) {
        state_groups[order[smaller_start + j]] = new_group_index;
      }
      worklist[work_tail++] = new_group_index;
    }
  }

  for (uint16_t group_index = 0; group_index < group_count; ++group_index) {
    const loom_scf_forwarding_group_t* group = &groups[group_index];
    uint16_t representative = LOOM_SCF_FORWARDING_TERMINAL;
    for (uint16_t i = 0; i < group->count; ++i) {
      const uint16_t state = order[group->start + i];
      if (representative == LOOM_SCF_FORWARDING_TERMINAL ||
          state < representative) {
        representative = state;
      }
    }
    for (uint16_t i = 0; i < group->count; ++i) {
      out_representatives[order[group->start + i]] = representative;
    }
  }

  *out_class_count = group_count;
  return iree_ok_status();
}
