// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/amdgpu/delay_layout.h"

iree_status_t loom_amdgpu_delay_layout_builder_initialize(
    iree_host_size_t wait_state_count, iree_arena_allocator_t* arena,
    loom_amdgpu_delay_layout_builder_t* out_builder) {
  *out_builder = (loom_amdgpu_delay_layout_builder_t){
      .pending_state_index = IREE_HOST_SIZE_MAX,
  };
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, wait_state_count, sizeof(*out_builder->immediates),
      (void**)&out_builder->immediates));
  return iree_arena_allocate_array(arena, wait_state_count / 2,
                                   sizeof(*out_builder->spans),
                                   (void**)&out_builder->spans);
}

uint16_t loom_amdgpu_delay_layout_record(
    loom_amdgpu_delay_layout_builder_t* builder, iree_host_size_t state_index,
    uint32_t packet_index, uint64_t instruction_index, uint16_t immediate) {
  builder->immediates[state_index] = immediate;
  if (immediate > 0 && immediate <= 7) {
    if (builder->pending_state_index != IREE_HOST_SIZE_MAX) {
      const uint64_t distance =
          instruction_index - builder->pending_consumer_index;
      if (distance >= 1 && distance <= 5) {
        builder->immediates[builder->pending_state_index] |=
            (uint16_t)((distance << 4) | (immediate << 7));
        builder->immediates[state_index] = 0;
        builder->spans[builder->span_count++] =
            (loom_amdgpu_delay_layout_span_t){
                .first_packet_index = builder->pending_packet_index,
                .last_packet_index = packet_index,
            };
        loom_amdgpu_delay_layout_end_span(builder);
        return 0;
      }
    }
    builder->pending_state_index = state_index;
    builder->pending_consumer_index = instruction_index + 1;
    builder->pending_packet_index = packet_index;
  } else {
    // A two-selector hint replaces any unused dependency metadata.
    loom_amdgpu_delay_layout_end_span(builder);
  }
  return immediate;
}

bool loom_amdgpu_delay_layout_allows_island(
    const loom_amdgpu_delay_layout_t* layout, uint32_t packet_index,
    iree_host_size_t* span_cursor) {
  while (*span_cursor < layout->span_count &&
         packet_index > layout->spans[*span_cursor].last_packet_index) {
    ++*span_cursor;
  }
  return *span_cursor == layout->span_count ||
         packet_index <= layout->spans[*span_cursor].first_packet_index;
}
