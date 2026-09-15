// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU wait-packet target views and compact lookup helpers.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_PACKET_TABLES_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_PACKET_TABLES_H_

#include "iree/base/api.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/ir/ir.h"
#include "loom/target/arch/amdgpu/planning/wait_packet_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_amdgpu_wait_packet_target_t {
  // Generated descriptor template rows available on this target.
  const loom_amdgpu_wait_packet_descriptor_template_t* descriptors;
  // Number of descriptor template rows available on this target.
  iree_host_size_t descriptor_count;
  // Dense lookup from descriptor ordinal to local descriptor-template index+1.
  const uint16_t* descriptor_lookup;
  // Number of descriptor ordinals addressable by descriptor_lookup.
  iree_host_size_t descriptor_lookup_count;
  // Generated best descriptor-template rows indexed by logical counter mask.
  const loom_amdgpu_wait_packet_selection_template_t* selections;
  // Number of generated counter-mask selection rows.
  iree_host_size_t selection_count;
  // Maximum immediate template count for any available wait descriptor.
  iree_host_size_t max_descriptor_immediate_count;
} loom_amdgpu_wait_packet_target_t;

// Populates the generated wait-packet rows available on |descriptor_set|.
void loom_amdgpu_wait_packet_analyze_target(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_amdgpu_wait_packet_target_t* out_target);

// Returns the immediate template row for |packet_descriptor|'s local immediate
// |immediate_index|.
const loom_amdgpu_wait_packet_descriptor_immediate_template_t*
loom_amdgpu_wait_packet_descriptor_immediate(
    const loom_amdgpu_wait_packet_descriptor_template_t* packet_descriptor,
    uint16_t immediate_index);

// Resolves a generated wait-packet descriptor template to the descriptor row in
// |descriptor_set|.
const loom_low_descriptor_t* loom_amdgpu_wait_packet_resolve_descriptor(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_wait_packet_descriptor_template_t* packet_descriptor);

// Concrete logical-counter bounds decoded once from an explicit wait packet.
typedef struct loom_amdgpu_wait_packet_bounds_t {
  // Remaining-work upper bounds by logical counter slot. UINT16_MAX leaves
  // the counter unconstrained; zero proves full completion.
  uint16_t target_counts[LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT];
} loom_amdgpu_wait_packet_bounds_t;

// Decodes an explicit packet's concrete immediate attributes into retained
// logical-counter bounds. Returns the mask of fully drained counters; partial
// bounds and unconstrained fields never establish a counter reset.
uint32_t loom_amdgpu_wait_packet_decode_bounds(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_descriptor_t* descriptor,
    const loom_amdgpu_wait_packet_target_t* target, const loom_module_t* module,
    const loom_op_t* op, loom_amdgpu_wait_packet_bounds_t* out_bounds);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_PACKET_TABLES_H_
