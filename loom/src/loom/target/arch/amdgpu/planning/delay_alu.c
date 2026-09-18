// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/delay_alu.h"

#define LOOM_AMDGPU_DELAY_ALU_VALU_MAX 5u
#define LOOM_AMDGPU_DELAY_ALU_VALU_CYCLES 4u
#define LOOM_AMDGPU_DELAY_ALU_TRANS_MAX 4u
#define LOOM_AMDGPU_DELAY_ALU_SALU_CYCLES_MAX 4u
#define LOOM_AMDGPU_DELAY_ALU_SALU_BASE 8u

uint16_t loom_amdgpu_delay_alu_latency_cycles(
    loom_amdgpu_delay_alu_type_t type, uint16_t schedule_latency_cycles) {
  switch (type) {
    case LOOM_AMDGPU_DELAY_ALU_TYPE_VALU:
      return schedule_latency_cycles > LOOM_AMDGPU_DELAY_ALU_VALU_CYCLES
                 ? schedule_latency_cycles
                 : LOOM_AMDGPU_DELAY_ALU_VALU_CYCLES;
    case LOOM_AMDGPU_DELAY_ALU_TYPE_TRANS:
    case LOOM_AMDGPU_DELAY_ALU_TYPE_SALU:
      return schedule_latency_cycles;
    case LOOM_AMDGPU_DELAY_ALU_TYPE_OTHER:
    default:
      return 0;
  }
}

static uint8_t loom_amdgpu_delay_alu_clamp_cycles(uint16_t cycle_count) {
  return cycle_count > UINT8_MAX ? UINT8_MAX : (uint8_t)cycle_count;
}

loom_amdgpu_delay_alu_info_t loom_amdgpu_delay_alu_make_info(
    const loom_amdgpu_delay_alu_state_t* state, uint64_t position,
    loom_amdgpu_delay_alu_type_t type, uint16_t latency_cycles,
    uint32_t producer_node) {
  const uint8_t cycles = loom_amdgpu_delay_alu_clamp_cycles(latency_cycles);
  loom_amdgpu_delay_alu_info_t info = {
      .epoch = state->epoch,
  };
  switch (type) {
    case LOOM_AMDGPU_DELAY_ALU_TYPE_VALU:
      info.valu.required_cycles = cycles;
      info.valu.producer_node = producer_node;
      info.valu.number_base = state->valu_count;
      break;
    case LOOM_AMDGPU_DELAY_ALU_TYPE_TRANS:
      info.trans.required_cycles = cycles;
      info.trans.producer_node = producer_node;
      info.trans.number_base = state->trans_count;
      info.trans.valu_number_base = state->valu_count;
      break;
    case LOOM_AMDGPU_DELAY_ALU_TYPE_SALU: {
      const uint8_t salu_cycles = cycles > LOOM_AMDGPU_DELAY_ALU_SALU_CYCLES_MAX
                                      ? LOOM_AMDGPU_DELAY_ALU_SALU_CYCLES_MAX
                                      : cycles;
      info.salu.required_cycles = salu_cycles;
      info.salu.producer_node = producer_node;
      info.salu.producer_position = position;
      break;
    }
    case LOOM_AMDGPU_DELAY_ALU_TYPE_OTHER:
    default:
      break;
  }
  return info;
}

static void loom_amdgpu_delay_alu_update_match(
    uint16_t required_cycle_count, uint16_t observed_cycle_count,
    uint16_t residual_cycle_count, uint32_t producer_node,
    loom_amdgpu_delay_alu_match_t* match) {
  if (residual_cycle_count == 0 || residual_cycle_count <= match->cycle_count) {
    return;
  }
  *match = (loom_amdgpu_delay_alu_match_t){
      .producer_node = producer_node,
      .required_cycle_count = required_cycle_count,
      .observed_cycle_count = observed_cycle_count,
      .cycle_count = residual_cycle_count,
      .delay_alu_immediate = match->delay_alu_immediate,
  };
}

static bool loom_amdgpu_delay_alu_info_is_current(
    const loom_amdgpu_delay_alu_state_t* state,
    const loom_amdgpu_delay_alu_info_t* info) {
  return info->epoch == state->epoch;
}

static bool loom_amdgpu_delay_alu_cycle_delta(uint64_t position,
                                              uint8_t required_cycles,
                                              uint64_t producer_position,
                                              uint16_t* out_observed_cycles,
                                              uint16_t* out_residual_cycles) {
  *out_observed_cycles = 0;
  *out_residual_cycles = 0;
  if (required_cycles == 0) {
    return false;
  }
  const uint64_t elapsed =
      position >= producer_position ? position - producer_position : 0;
  if (elapsed >= required_cycles) {
    return false;
  }
  *out_observed_cycles = (uint16_t)elapsed;
  *out_residual_cycles = (uint16_t)(required_cycles - elapsed);
  return true;
}

static bool loom_amdgpu_delay_alu_counter_delta(uint64_t counter, uint64_t base,
                                                uint8_t maximum_delta,
                                                uint8_t* out_delta) {
  *out_delta = 0;
  if (counter < base) {
    return false;
  }
  const uint64_t delta = counter - base;
  if (delta >= maximum_delta) {
    return false;
  }
  *out_delta = (uint8_t)delta;
  return true;
}

// VALU/TRANS delay selectors identify recent producer packets by their ALU
// issue class. Ordinary scalar packets between producer and consumer do not
// advance that selector, so observed progress must come from the class counter
// rather than the generic instruction position.
static bool loom_amdgpu_delay_alu_class_delta(uint8_t required_cycles,
                                              uint64_t counter, uint64_t base,
                                              uint8_t maximum_delta,
                                              uint16_t* out_observed_cycles,
                                              uint16_t* out_residual_cycles,
                                              uint8_t* out_number) {
  *out_observed_cycles = 0;
  *out_residual_cycles = 0;
  *out_number = 0;
  if (required_cycles == 0 ||
      !loom_amdgpu_delay_alu_counter_delta(counter, base, maximum_delta,
                                           out_number) ||
      *out_number > required_cycles) {
    return false;
  }
  if (*out_number == required_cycles) {
    *out_observed_cycles = (uint16_t)(required_cycles - 1);
    *out_residual_cycles = 1;
    return true;
  }
  *out_observed_cycles = *out_number;
  *out_residual_cycles = (uint16_t)(required_cycles - *out_number);
  return true;
}

static bool loom_amdgpu_delay_alu_valu_delta(
    const loom_amdgpu_delay_alu_state_t* state,
    const loom_amdgpu_delay_alu_info_t* info, uint16_t* out_observed_cycles,
    uint16_t* out_residual_cycles, uint8_t* out_valu_number) {
  return loom_amdgpu_delay_alu_info_is_current(state, info) &&
         loom_amdgpu_delay_alu_class_delta(
             info->valu.required_cycles, state->valu_count,
             info->valu.number_base, LOOM_AMDGPU_DELAY_ALU_VALU_MAX,
             out_observed_cycles, out_residual_cycles, out_valu_number) &&
         !(state->completed.valu & (1u << *out_valu_number));
}

static bool loom_amdgpu_delay_alu_trans_delta(
    const loom_amdgpu_delay_alu_state_t* state,
    const loom_amdgpu_delay_alu_info_t* info, uint16_t* out_observed_cycles,
    uint16_t* out_residual_cycles, uint8_t* out_trans_number,
    uint8_t* out_trans_valu_number) {
  return loom_amdgpu_delay_alu_info_is_current(state, info) &&
         loom_amdgpu_delay_alu_class_delta(
             info->trans.required_cycles, state->trans_count,
             info->trans.number_base, LOOM_AMDGPU_DELAY_ALU_TRANS_MAX,
             out_observed_cycles, out_residual_cycles, out_trans_number) &&
         !(state->completed.trans & (1u << *out_trans_number)) &&
         loom_amdgpu_delay_alu_counter_delta(state->valu_count,
                                             info->trans.valu_number_base,
                                             UINT8_MAX, out_trans_valu_number);
}

static bool loom_amdgpu_delay_alu_salu_delta(
    const loom_amdgpu_delay_alu_state_t* state, uint64_t position,
    const loom_amdgpu_delay_alu_info_t* info, uint16_t* out_observed_cycles,
    uint16_t* out_residual_cycles) {
  return loom_amdgpu_delay_alu_info_is_current(state, info) &&
         loom_amdgpu_delay_alu_cycle_delta(
             position, info->salu.required_cycles, info->salu.producer_position,
             out_observed_cycles, out_residual_cycles) &&
         *out_residual_cycles < LOOM_AMDGPU_DELAY_ALU_SALU_CYCLES_MAX;
}

static bool loom_amdgpu_delay_alu_candidate_is_better(
    const loom_amdgpu_delay_alu_candidate_t* source,
    const loom_amdgpu_delay_alu_candidate_t* target) {
  if (source->residual_cycle_count != target->residual_cycle_count) {
    return source->residual_cycle_count > target->residual_cycle_count;
  }
  if (source->required_cycle_count != target->required_cycle_count) {
    return source->required_cycle_count > target->required_cycle_count;
  }
  if (source->observed_cycle_count != target->observed_cycle_count) {
    return source->observed_cycle_count < target->observed_cycle_count;
  }
  return source->dependency_code < target->dependency_code;
}

static loom_amdgpu_delay_alu_type_t loom_amdgpu_delay_alu_dependency_class(
    uint16_t dependency_code) {
  if (dependency_code <= 4) {
    return LOOM_AMDGPU_DELAY_ALU_TYPE_VALU;
  }
  if (dependency_code <= 7) {
    return LOOM_AMDGPU_DELAY_ALU_TYPE_TRANS;
  }
  return LOOM_AMDGPU_DELAY_ALU_TYPE_SALU;
}

static bool loom_amdgpu_delay_alu_candidate_matches(
    const loom_amdgpu_delay_alu_candidate_t* lhs,
    const loom_amdgpu_delay_alu_candidate_t* rhs) {
  // Moves in one structural packet share an issue class and latency, so its
  // strongest residual subsumes earlier moves. VOPD component results have
  // distinct producer nodes but the same dependency selector because they
  // issue in one native packet; one selector waits for both.
  const bool same_dependency_class =
      loom_amdgpu_delay_alu_dependency_class(lhs->dependency_code) ==
      loom_amdgpu_delay_alu_dependency_class(rhs->dependency_code);
  return same_dependency_class &&
         (lhs->producer_node == rhs->producer_node ||
          lhs->dependency_code == rhs->dependency_code);
}

static void loom_amdgpu_delay_alu_sort_candidates(
    loom_amdgpu_delay_alu_accumulator_t* accumulator) {
  if (accumulator->candidate_count < 2) {
    return;
  }
  if (loom_amdgpu_delay_alu_candidate_is_better(&accumulator->candidates[1],
                                                &accumulator->candidates[0])) {
    const loom_amdgpu_delay_alu_candidate_t temporary =
        accumulator->candidates[0];
    accumulator->candidates[0] = accumulator->candidates[1];
    accumulator->candidates[1] = temporary;
  }
}

static void loom_amdgpu_delay_alu_add_candidate(
    loom_amdgpu_delay_alu_accumulator_t* accumulator, uint16_t dependency_code,
    uint16_t required_cycle_count, uint16_t observed_cycle_count,
    uint16_t residual_cycle_count, uint32_t producer_node) {
  if (residual_cycle_count == 0) {
    return;
  }
  loom_amdgpu_delay_alu_update_match(required_cycle_count, observed_cycle_count,
                                     residual_cycle_count, producer_node,
                                     &accumulator->fallback_match);
  const loom_amdgpu_delay_alu_candidate_t candidate = {
      .dependency_code = dependency_code,
      .producer_node = producer_node,
      .required_cycle_count = required_cycle_count,
      .observed_cycle_count = observed_cycle_count,
      .residual_cycle_count = residual_cycle_count,
  };
  for (uint8_t i = 0; i < accumulator->candidate_count; ++i) {
    if (!loom_amdgpu_delay_alu_candidate_matches(&candidate,
                                                 &accumulator->candidates[i])) {
      continue;
    }
    if (loom_amdgpu_delay_alu_candidate_is_better(
            &candidate, &accumulator->candidates[i])) {
      accumulator->candidates[i] = candidate;
      loom_amdgpu_delay_alu_sort_candidates(accumulator);
    }
    return;
  }
  if (accumulator->candidate_count < LOOM_AMDGPU_DELAY_ALU_SELECTOR_CAPACITY) {
    accumulator->candidates[accumulator->candidate_count++] = candidate;
    loom_amdgpu_delay_alu_sort_candidates(accumulator);
    return;
  }
  accumulator->flags |=
      LOOM_AMDGPU_DELAY_ALU_ACCUMULATOR_FLAG_UNENCODED_CANDIDATES;
  const uint8_t worst_index = accumulator->candidate_count - 1;
  if (loom_amdgpu_delay_alu_candidate_is_better(
          &candidate, &accumulator->candidates[worst_index])) {
    accumulator->candidates[worst_index] = candidate;
    loom_amdgpu_delay_alu_sort_candidates(accumulator);
  }
}

void loom_amdgpu_delay_alu_accumulate_info(
    const loom_amdgpu_delay_alu_state_t* state, uint64_t position,
    const loom_amdgpu_delay_alu_info_t* info,
    loom_amdgpu_delay_alu_accumulator_t* accumulator) {
  uint16_t observed_cycles = 0;
  uint16_t residual_cycles = 0;
  uint8_t trans_number = 0;
  uint8_t trans_valu_number = 0;
  const bool has_trans = loom_amdgpu_delay_alu_trans_delta(
      state, info, &observed_cycles, &residual_cycles, &trans_number,
      &trans_valu_number);
  if (has_trans) {
    loom_amdgpu_delay_alu_add_candidate(
        accumulator, (uint16_t)(4u + trans_number), info->trans.required_cycles,
        observed_cycles, residual_cycles, info->trans.producer_node);
  }
  uint8_t valu_number = 0;
  if (loom_amdgpu_delay_alu_valu_delta(state, info, &observed_cycles,
                                       &residual_cycles, &valu_number) &&
      (!has_trans || valu_number <= trans_valu_number)) {
    loom_amdgpu_delay_alu_add_candidate(
        accumulator, valu_number, info->valu.required_cycles, observed_cycles,
        residual_cycles, info->valu.producer_node);
  }
  if (loom_amdgpu_delay_alu_salu_delta(state, position, info, &observed_cycles,
                                       &residual_cycles)) {
    const uint16_t salu_code =
        (uint16_t)(residual_cycles + LOOM_AMDGPU_DELAY_ALU_SALU_BASE);
    loom_amdgpu_delay_alu_add_candidate(
        accumulator, salu_code, info->salu.required_cycles, observed_cycles,
        residual_cycles, info->salu.producer_node);
  }
}

loom_amdgpu_delay_alu_match_t loom_amdgpu_delay_alu_select(
    const loom_amdgpu_delay_alu_accumulator_t* accumulator) {
  loom_amdgpu_delay_alu_match_t match = accumulator->fallback_match;
  if (match.cycle_count == 0 ||
      iree_any_bit_set(
          accumulator->flags,
          LOOM_AMDGPU_DELAY_ALU_ACCUMULATOR_FLAG_UNENCODED_CANDIDATES)) {
    return match;
  }
  if (accumulator->candidate_count >= 1) {
    match.delay_alu_immediate |= accumulator->candidates[0].dependency_code;
  }
  if (accumulator->candidate_count >= 2) {
    match.delay_alu_immediate |=
        (uint16_t)(accumulator->candidates[1].dependency_code << 7);
  }
  return match;
}

void loom_amdgpu_delay_alu_advance(loom_amdgpu_delay_alu_state_t* state,
                                   loom_amdgpu_delay_alu_type_t type,
                                   uint64_t instruction_count) {
  if (instruction_count == 0) {
    return;
  }
  switch (type) {
    case LOOM_AMDGPU_DELAY_ALU_TYPE_VALU:
      state->valu_count += instruction_count;
      state->completed.valu = instruction_count < LOOM_AMDGPU_DELAY_ALU_VALU_MAX
                                  ? state->completed.valu << instruction_count
                                  : 0;
      break;
    case LOOM_AMDGPU_DELAY_ALU_TYPE_TRANS:
      state->trans_count += instruction_count;
      state->completed.trans =
          instruction_count < LOOM_AMDGPU_DELAY_ALU_TRANS_MAX
              ? state->completed.trans << instruction_count
              : 0;
      break;
    case LOOM_AMDGPU_DELAY_ALU_TYPE_SALU:
    case LOOM_AMDGPU_DELAY_ALU_TYPE_OTHER:
    default:
      break;
  }
}

bool loom_amdgpu_delay_alu_reset(loom_amdgpu_delay_alu_state_t* state) {
  ++state->epoch;
  state->valu_count = 0;
  state->trans_count = 0;
  state->completed.valu = 0;
  state->completed.trans = 0;
  if (state->epoch != 0) {
    return false;
  }
  state->epoch = 1;
  return true;
}

void loom_amdgpu_delay_alu_complete(loom_amdgpu_delay_alu_state_t* state,
                                    uint16_t immediate) {
  // Each selector completes the native producer, including its other results.
  // SALU selectors encode cycles instead of producer identities.
  for (uint16_t shift = 0; shift <= 7; shift += 7) {
    const uint16_t selector = (immediate >> shift) & 15u;
    if (selector >= 1 && selector < LOOM_AMDGPU_DELAY_ALU_VALU_MAX) {
      state->completed.valu |= 1u << selector;
    } else if (selector >= 5 && selector < 8) {
      state->completed.trans |= 1u << (selector - 4);
    }
  }
}
