// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/region.h"

#include <string.h>

#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "IREE shared-memory transport requires little-endian hosts"
#endif  // !IREE_ENDIANNESS_LITTLE

iree_status_t iree_net_shm_region_calculate_layout(
    iree_net_shm_region_options_t options,
    iree_net_shm_region_layout_t* out_layout) {
  memset(out_layout, 0, sizeof(*out_layout));
  if (!options.endpoint_count || !options.slot_count ||
      options.slot_count > IREE_ATOMIC_FREELIST_MAX_COUNT ||
      !options.slot_capacity) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM requires endpoints, 1..65534 slots per "
                            "direction, and nonzero slot capacity");
  }

  // At most N descriptors own slots simultaneously. Include one extra entry
  // for the queue's wrap padding so descriptor admission cannot bottleneck a
  // successfully acquired payload slot.
  const uint32_t descriptor_entry_size = (uint32_t)iree_host_align(
      sizeof(uint32_t) + sizeof(iree_net_shm_descriptor_t),
      IREE_MPSC_QUEUE_DEFAULT_ENTRY_ALIGNMENT);
  uint32_t descriptor_capacity = IREE_MPSC_QUEUE_MIN_CAPACITY;
  const uint32_t descriptor_bytes =
      (options.slot_count + 1) * descriptor_entry_size;
  while (descriptor_capacity < descriptor_bytes) {
    descriptor_capacity *= 2;
  }

  iree_net_shm_region_layout_t layout = {0};
  layout.options = options;
  layout.descriptor_capacity = descriptor_capacity;
  layout.links_offset = 2 * IREE_NET_SHM_REGION_ALIGNMENT;
  if (!iree_host_size_checked_mul(options.endpoint_count, 2,
                                  &layout.direction_count) ||
      !iree_host_size_checked_mul_add(layout.links_offset, options.slot_count,
                                      sizeof(iree_atomic_freelist_slot_t),
                                      &layout.descriptors_offset) ||
      !iree_host_size_checked_align(layout.descriptors_offset,
                                    IREE_NET_SHM_REGION_ALIGNMENT,
                                    &layout.descriptors_offset) ||
      !iree_host_size_checked_add(
          layout.descriptors_offset,
          iree_mpsc_queue_required_size(descriptor_capacity),
          &layout.payload_offset) ||
      !iree_host_size_checked_align(options.slot_capacity,
                                    IREE_NET_SHM_REGION_ALIGNMENT,
                                    &layout.slot_stride) ||
      !iree_host_size_checked_mul_add(layout.payload_offset, options.slot_count,
                                      layout.slot_stride,
                                      &layout.direction_stride) ||
      !iree_host_size_checked_mul_add(
          2 * IREE_NET_SHM_REGION_ALIGNMENT, layout.direction_count,
          layout.direction_stride, &layout.total_size)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "SHM geometry exceeds the host address space");
  }
  *out_layout = layout;
  return iree_ok_status();
}

static iree_status_t iree_net_shm_region_validate_storage(
    const iree_net_shm_region_layout_t* layout, iree_byte_span_t storage) {
  if (!storage.data ||
      (uintptr_t)storage.data % IREE_NET_SHM_REGION_ALIGNMENT != 0 ||
      storage.data_length < layout->total_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM mapping must be 64-byte aligned and contain "
                            "at least %" PRIhsz " bytes",
                            layout->total_size);
  }
  return iree_ok_status();
}

static uint8_t* iree_net_shm_region_bind_direction(
    const iree_net_shm_region_layout_t* layout, uint8_t* storage,
    iree_host_size_t index, iree_net_shm_direction_t* out_direction) {
  uint8_t* base = storage + 2 * IREE_NET_SHM_REGION_ALIGNMENT +
                  index * layout->direction_stride;
  out_direction->free_slots = (iree_atomic_freelist_t*)base;
  out_direction->consumed_position =
      (iree_atomic_uint64_t*)(base + IREE_NET_SHM_REGION_ALIGNMENT);
  out_direction->receiver_closed =
      (iree_atomic_int32_t*)(base + IREE_NET_SHM_REGION_ALIGNMENT +
                             sizeof(iree_atomic_uint64_t));
  out_direction->links =
      (iree_atomic_freelist_slot_t*)(base + layout->links_offset);
  out_direction->payload = base + layout->payload_offset;
  return base + layout->descriptors_offset;
}

iree_status_t iree_net_shm_region_initialize(
    const iree_net_shm_region_layout_t* layout, iree_byte_span_t storage,
    iree_net_shm_direction_t* out_directions) {
  IREE_RETURN_IF_ERROR(iree_net_shm_region_validate_storage(layout, storage));
  for (uint32_t side = 0; side < 2; ++side) {
    iree_notification_state_initialize(
        iree_net_shm_region_notification_state(storage.data, side));
  }
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < layout->direction_count && iree_status_is_ok(status); ++i) {
    iree_net_shm_direction_t* direction = &out_directions[i];
    uint8_t* descriptors =
        iree_net_shm_region_bind_direction(layout, storage.data, i, direction);
    iree_atomic_store(direction->consumed_position, 0,
                      iree_memory_order_relaxed);
    iree_atomic_store(direction->receiver_closed, 0, iree_memory_order_relaxed);
    status = iree_atomic_freelist_initialize(
        direction->links, layout->options.slot_count, direction->free_slots);
    if (iree_status_is_ok(status)) {
      status = iree_mpsc_queue_initialize(
          descriptors,
          iree_mpsc_queue_required_size(layout->descriptor_capacity),
          layout->descriptor_capacity, &direction->descriptors);
    }
  }
  return status;
}

iree_status_t iree_net_shm_region_open(
    const iree_net_shm_region_layout_t* layout, iree_byte_span_t storage,
    iree_net_shm_direction_t* out_directions) {
  IREE_RETURN_IF_ERROR(iree_net_shm_region_validate_storage(layout, storage));
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < layout->direction_count && iree_status_is_ok(status); ++i) {
    uint8_t* descriptors = iree_net_shm_region_bind_direction(
        layout, storage.data, i, &out_directions[i]);
    iree_mpsc_queue_t* queue = &out_directions[i].descriptors;
    status = iree_mpsc_queue_open(
        descriptors, iree_mpsc_queue_required_size(layout->descriptor_capacity),
        queue);
    if (iree_status_is_ok(status) &&
        (queue->capacity != layout->descriptor_capacity ||
         queue->entry_alignment != IREE_MPSC_QUEUE_DEFAULT_ENTRY_ALIGNMENT)) {
      status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "SHM descriptor queue disagrees with bootstrap "
                                "geometry at direction %" PRIhsz,
                                i);
    }
  }
  return status;
}
