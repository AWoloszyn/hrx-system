// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Recent native ALU producer timing and S_DELAY_ALU selector selection.
// Per-location producer facts remain owned by the packet planner. This state
// carries issue-class counters and completion of exact native producers across
// uses of their different results, without scanning or clearing other
// locations.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_DELAY_ALU_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_DELAY_ALU_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_amdgpu_delay_alu_type_e {
  LOOM_AMDGPU_DELAY_ALU_TYPE_OTHER = 0,
  LOOM_AMDGPU_DELAY_ALU_TYPE_VALU = 1,
  LOOM_AMDGPU_DELAY_ALU_TYPE_TRANS = 2,
  LOOM_AMDGPU_DELAY_ALU_TYPE_SALU = 3,
} loom_amdgpu_delay_alu_type_t;

enum { LOOM_AMDGPU_DELAY_ALU_SELECTOR_CAPACITY = 2 };

typedef struct loom_amdgpu_delay_alu_state_t {
  // Active epoch for delay-ALU producer state.
  uint32_t epoch;
  // Number of delay-tracked VALU packets issued in the active epoch.
  uint64_t valu_count;
  // Number of delay-tracked TRANS packets issued in the active epoch.
  uint64_t trans_count;
  // Completed native producers indexed by their current dependency selector.
  struct {
    // Bit N records completion of VALU_DEP_N in the current epoch.
    uint8_t valu;
    // Bit N records completion of TRANS32_DEP_N in the current epoch.
    uint8_t trans;
  } completed;
} loom_amdgpu_delay_alu_state_t;

typedef struct loom_amdgpu_delay_alu_info_t {
  // Builder epoch in which this producer state was recorded.
  uint32_t epoch;
  // Most recent VALU producer of the physical location.
  struct {
    // Original modeled producer latency.
    uint8_t required_cycles;
    // Schedule node that produced the write.
    uint32_t producer_node;
    // VALU issue count immediately before the producer.
    uint64_t number_base;
  } valu;
  // Most recent transcendental producer of the physical location.
  struct {
    // Original modeled producer latency.
    uint8_t required_cycles;
    // Schedule node that produced the write.
    uint32_t producer_node;
    // TRANS issue count immediately before the producer.
    uint64_t number_base;
    // VALU issue count immediately before the producer.
    uint64_t valu_number_base;
  } trans;
  // Most recent scalar producer of the physical location.
  struct {
    // Original modeled producer latency.
    uint8_t required_cycles;
    // Schedule node that produced the write.
    uint32_t producer_node;
    // Instruction position immediately before the producer.
    uint64_t producer_position;
  } salu;
} loom_amdgpu_delay_alu_info_t;

// Strongest residual dependency and the selected encoded hint. A zero cycle
// count needs no action. A nonzero cycle count with a zero immediate uses
// S_NOP.
typedef struct loom_amdgpu_delay_alu_match_t {
  // Schedule node that produced the strongest dependency.
  uint32_t producer_node;
  // Original modeled producer latency.
  uint16_t required_cycle_count;
  // Progress observed before the consumer.
  uint16_t observed_cycle_count;
  // Residual cycles before the consumer.
  uint16_t cycle_count;
  // Packed S_DELAY_ALU selectors, or zero when the candidates require S_NOP.
  uint16_t delay_alu_immediate;
} loom_amdgpu_delay_alu_match_t;

typedef enum loom_amdgpu_delay_alu_accumulator_flag_bits_e {
  LOOM_AMDGPU_DELAY_ALU_ACCUMULATOR_FLAG_UNENCODED_CANDIDATES = 1u << 0,
} loom_amdgpu_delay_alu_accumulator_flag_bits_t;
typedef uint8_t loom_amdgpu_delay_alu_accumulator_flags_t;

typedef struct loom_amdgpu_delay_alu_candidate_t {
  // Target-format S_DELAY_ALU INSTID selector for the producer.
  uint16_t dependency_code;
  // Schedule node that produced the dependency.
  uint32_t producer_node;
  // Required target progress before the current consumer.
  uint16_t required_cycle_count;
  // Target progress already supplied before the current consumer.
  uint16_t observed_cycle_count;
  // Additional cycles required before the current consumer.
  uint16_t residual_cycle_count;
} loom_amdgpu_delay_alu_candidate_t;

typedef struct loom_amdgpu_delay_alu_accumulator_t {
  // Best candidates that fit in one S_DELAY_ALU immediate.
  loom_amdgpu_delay_alu_candidate_t
      candidates[LOOM_AMDGPU_DELAY_ALU_SELECTOR_CAPACITY];
  // Number of populated candidates.
  uint8_t candidate_count;
  // Accumulation flags.
  loom_amdgpu_delay_alu_accumulator_flags_t flags;
  // Strongest dependency if the candidate set must fall back to S_NOP.
  loom_amdgpu_delay_alu_match_t fallback_match;
} loom_amdgpu_delay_alu_accumulator_t;

// Starts an empty producer epoch. Returns true on epoch wrap, requiring the
// owner to clear its per-location producer facts before recording new results.
bool loom_amdgpu_delay_alu_reset(loom_amdgpu_delay_alu_state_t* state);

// Advances the native producer selector window by the emitted instruction
// count.
void loom_amdgpu_delay_alu_advance(loom_amdgpu_delay_alu_state_t* state,
                                   loom_amdgpu_delay_alu_type_t type,
                                   uint64_t instruction_count);

// Records completion of the exact producers selected by an emitted hint.
// Both selectors apply to the next consumer (INSTSKIP is zero). No completion
// is inferred for other producers or from SALU cycle-count selectors.
void loom_amdgpu_delay_alu_complete(loom_amdgpu_delay_alu_state_t* state,
                                    uint16_t immediate);

// Applies the minimum ALU hint latency to the selected schedule class latency.
uint16_t loom_amdgpu_delay_alu_latency_cycles(loom_amdgpu_delay_alu_type_t type,
                                              uint16_t schedule_latency_cycles);

// Captures the native producer identity before its issue-class clock advances.
loom_amdgpu_delay_alu_info_t loom_amdgpu_delay_alu_make_info(
    const loom_amdgpu_delay_alu_state_t* state, uint64_t position,
    loom_amdgpu_delay_alu_type_t type, uint16_t latency_cycles,
    uint32_t producer_node);

// Adds one physical operand's current dependencies to a zero-initialized or
// partially accumulated candidate set for the next native consumer.
void loom_amdgpu_delay_alu_accumulate_info(
    const loom_amdgpu_delay_alu_state_t* state, uint64_t position,
    const loom_amdgpu_delay_alu_info_t* info,
    loom_amdgpu_delay_alu_accumulator_t* accumulator);

// Selects a hint for at most two distinct native producers, or the strongest
// scalar no-op delay when the candidate set exceeds the encoded capacity.
loom_amdgpu_delay_alu_match_t loom_amdgpu_delay_alu_select(
    const loom_amdgpu_delay_alu_accumulator_t* accumulator);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_DELAY_ALU_H_
