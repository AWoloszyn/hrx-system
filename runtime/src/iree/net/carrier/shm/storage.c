// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/storage.h"

static iree_status_t iree_net_shm_storage_create_state(
    const iree_net_shm_region_layout_t* layout, uint32_t side,
    iree_allocator_t host_allocator, iree_net_shm_storage_t** out_storage) {
  *out_storage = NULL;
  // Native page rounding must not wrap before create/open_handle sees it.
  if (iree_shm_required_size(layout->total_size) < layout->total_size) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "SHM page-rounded extent exceeds the host size");
  }
  iree_host_size_t allocation_size = 0;
  iree_host_size_t directions_offset = 0;
  iree_host_size_t endpoints_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_shm_storage_t), &allocation_size,
      IREE_STRUCT_FIELD(layout->direction_count, iree_net_shm_direction_t,
                        &directions_offset),
      IREE_STRUCT_FIELD(layout->options.endpoint_count,
                        iree_net_shm_storage_endpoint_t, &endpoints_offset)));
  iree_net_shm_storage_t* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, allocation_size, (void**)&storage));
  iree_atomic_ref_count_init(&storage->ref_count);
  storage->host_allocator = host_allocator;
  storage->mapping.handle = IREE_SHM_HANDLE_INVALID;
  storage->layout = *layout;
  storage->side = side;
  storage->directions =
      (iree_net_shm_direction_t*)((uint8_t*)storage + directions_offset);
  storage->endpoints =
      (iree_net_shm_storage_endpoint_t*)((uint8_t*)storage + endpoints_offset);
  iree_slim_mutex_initialize(&storage->failure.mutex);
  for (uint32_t i = 0; i < layout->options.endpoint_count; ++i) {
    storage->endpoints[i].storage = storage;
    storage->endpoints[i].incoming =
        &storage->directions[(iree_host_size_t)i * 2 + (side ^ 1u)];
  }
  *out_storage = storage;
  return iree_ok_status();
}

void iree_net_shm_storage_retain(iree_net_shm_storage_t* storage) {
  if (!storage) {
    return;
  }
  iree_atomic_ref_count_inc(&storage->ref_count);
}

void iree_net_shm_storage_release(iree_net_shm_storage_t* storage) {
  if (!storage) {
    return;
  }
  if (iree_atomic_ref_count_dec(&storage->ref_count) != 1) {
    return;
  }
  iree_allocator_t host_allocator = storage->host_allocator;
  IREE_ASSERT(!storage->failure.callback.fn,
              "SHM storage destroyed before connection detach");
  iree_status_free(storage->failure.status);
  iree_slim_mutex_deinitialize(&storage->failure.mutex);
  for (uint32_t i = 0; i < 2; ++i) {
    iree_async_event_native_deinitialize(&storage->wakes[i]);
  }
  iree_shm_close(&storage->mapping);
  iree_allocator_free(host_allocator, storage);
}

iree_status_t iree_net_shm_storage_create(
    const iree_net_shm_region_layout_t* layout, iree_allocator_t host_allocator,
    iree_net_shm_storage_t** out_storage) {
  *out_storage = NULL;
  iree_net_shm_storage_t* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_net_shm_storage_create_state(layout, 0, host_allocator, &storage));
  iree_status_t status =
      iree_shm_create(NULL, layout->total_size, &storage->mapping);
  for (uint32_t i = 0; i < 2 && iree_status_is_ok(status); ++i) {
    status = iree_async_event_native_initialize(&storage->wakes[i]);
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_shm_region_initialize(
        layout,
        iree_make_byte_span(storage->mapping.base, storage->mapping.size),
        storage->directions);
  }
  if (iree_status_is_ok(status)) {
    *out_storage = storage;
  } else {
    iree_net_shm_storage_release(storage);
  }
  return status;
}

void iree_net_shm_storage_export(
    iree_net_shm_storage_t* storage,
    iree_async_primitive_t out_handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT]) {
#if defined(IREE_PLATFORM_WINDOWS)
  out_handles[0] =
      iree_async_primitive_from_win32_handle(storage->mapping.handle.value);
#elif defined(IREE_ASYNC_HAVE_FD)
  out_handles[0] =
      iree_async_primitive_from_fd((int)storage->mapping.handle.value);
#else
  out_handles[0] = iree_async_primitive_none();
#endif
  for (uint32_t side = 0; side < 2; ++side) {
#if IREE_NET_SHM_STORAGE_HANDLE_COUNT == 3
    out_handles[1 + side] = storage->wakes[side].wait_primitive;
#else
    out_handles[1 + side * 2] = storage->wakes[side].wait_primitive;
    out_handles[2 + side * 2] = storage->wakes[side].signal_primitive;
#endif
  }
}

iree_status_t iree_net_shm_storage_import(
    const iree_net_shm_region_layout_t* layout,
    iree_async_primitive_t handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT],
    iree_allocator_t host_allocator, iree_net_shm_storage_t** out_storage) {
  *out_storage = NULL;
  iree_net_shm_storage_t* storage = NULL;
  iree_status_t status =
      iree_net_shm_storage_create_state(layout, 1, host_allocator, &storage);
  iree_shm_handle_t mapping_handle = IREE_SHM_HANDLE_INVALID;
  if (iree_status_is_ok(status)) {
#if defined(IREE_PLATFORM_WINDOWS)
    mapping_handle.value = handles[0].value.win32_handle;
#elif defined(IREE_ASYNC_HAVE_FD)
    mapping_handle.value = (uint64_t)handles[0].value.fd;
#else
    status = iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                              "SHM native resource import is unavailable");
#endif
  }
  if (iree_status_is_ok(status)) {
    status = iree_shm_open_handle(mapping_handle,
                                  iree_shm_required_size(layout->total_size),
                                  &storage->mapping);
  }
  if (iree_status_is_ok(status)) {
    for (uint32_t side = 0; side < 2; ++side) {
#if IREE_NET_SHM_STORAGE_HANDLE_COUNT == 3
      storage->wakes[side].wait_primitive = handles[1 + side];
      storage->wakes[side].signal_primitive = handles[1 + side];
      handles[1 + side] = iree_async_primitive_none();
#else
      storage->wakes[side].wait_primitive = handles[1 + side * 2];
      storage->wakes[side].signal_primitive = handles[2 + side * 2];
      handles[1 + side * 2] = iree_async_primitive_none();
      handles[2 + side * 2] = iree_async_primitive_none();
#endif
    }
    status = iree_net_shm_region_open(
        layout,
        iree_make_byte_span(storage->mapping.base, storage->mapping.size),
        storage->directions);
  }
  for (uint32_t i = 0; i < IREE_NET_SHM_STORAGE_HANDLE_COUNT; ++i) {
    iree_async_primitive_close(&handles[i]);
  }
  if (iree_status_is_ok(status)) {
    *out_storage = storage;
  } else {
    iree_net_shm_storage_release(storage);
  }
  return status;
}

void iree_net_shm_storage_set_failure_callback(
    iree_net_shm_storage_t* storage,
    iree_net_shm_storage_failure_callback_t callback) {
  iree_slim_mutex_lock(&storage->failure.mutex);
  storage->failure.callback = callback;
  if (callback.fn && !iree_status_is_ok(storage->failure.status)) {
    callback.fn(callback.user_data);
  }
  iree_slim_mutex_unlock(&storage->failure.mutex);
}

void iree_net_shm_storage_fail(iree_net_shm_storage_t* storage,
                               iree_status_t status) {
  iree_slim_mutex_lock(&storage->failure.mutex);
  const bool first = iree_status_is_ok(storage->failure.status);
  storage->failure.status = iree_status_join(storage->failure.status, status);
  if (first && storage->failure.callback.fn) {
    storage->failure.callback.fn(storage->failure.callback.user_data);
  }
  iree_slim_mutex_unlock(&storage->failure.mutex);
}

iree_status_t iree_net_shm_storage_clone_failure(
    iree_net_shm_storage_t* storage) {
  iree_slim_mutex_lock(&storage->failure.mutex);
  iree_status_t status = iree_status_clone(storage->failure.status);
  iree_slim_mutex_unlock(&storage->failure.mutex);
  return status;
}

iree_status_t iree_net_shm_storage_signal(iree_net_shm_storage_t* storage,
                                          uint32_t side) {
  iree_atomic_fetch_add(iree_net_shm_region_epoch(storage->mapping.base, side),
                        1, iree_memory_order_release);
  return iree_async_event_native_set(&storage->wakes[side]);
}

static void iree_net_shm_storage_release_lease(
    void* user_data, iree_async_buffer_index_t index) {
  iree_net_shm_storage_endpoint_t* endpoint = user_data;
  iree_net_shm_storage_t* storage = endpoint->storage;
  iree_atomic_freelist_push(endpoint->incoming->free_slots,
                            endpoint->incoming->links, (uint16_t)index);
  iree_atomic_fetch_sub(&endpoint->retained_count, 1,
                        iree_memory_order_release);
  iree_status_t status =
      iree_net_shm_storage_signal(storage, storage->side ^ 1u);
  if (!iree_status_is_ok(status)) {
    iree_net_shm_storage_fail(storage, status);
  }
  iree_net_shm_storage_release(storage);
}

bool iree_net_shm_storage_try_lease(iree_net_shm_storage_endpoint_t* endpoint,
                                    uint16_t slot, uint32_t length,
                                    iree_async_buffer_lease_t* out_lease) {
  memset(out_lease, 0, sizeof(*out_lease));
  iree_net_shm_storage_t* storage = endpoint->storage;
  if ((uint32_t)iree_atomic_load(&endpoint->retained_count,
                                 iree_memory_order_acquire) >=
      storage->layout.options.slot_count - 1) {
    return false;
  }
  // Only this endpoint's poll owner increments; concurrent releases can only
  // make more retention capacity available after the admission check.
  iree_atomic_fetch_add(&endpoint->retained_count, 1,
                        iree_memory_order_relaxed);
  iree_net_shm_storage_retain(storage);
  *out_lease = (iree_async_buffer_lease_t){
      .span = iree_async_span_from_ptr(
          endpoint->incoming->payload + slot * storage->layout.slot_stride,
          length),
      .release = {iree_net_shm_storage_release_lease, endpoint},
      .buffer_index = slot,
  };
  return true;
}
