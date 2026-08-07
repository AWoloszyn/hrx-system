// Copyright 2025 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string.h>

#include "common/internal.h"

//===----------------------------------------------------------------------===//
// Device management
//===----------------------------------------------------------------------===//

iree_status_t iree_hal_streaming_device_count(iree_host_size_t* out_count) {
  IREE_ASSERT_ARGUMENT(out_count);
  iree_hal_streaming_device_registry_t* device_registry =
      iree_hal_streaming_device_registry();
  if (!device_registry) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "HAL stream layer not initialized");
  }

  *out_count = device_registry->device_count;
  return iree_ok_status();
}

static iree_status_t iree_hal_streaming_device_by_ordinal(
    iree_hal_streaming_device_ordinal_t ordinal,
    iree_hal_streaming_device_t** out_device) {
  IREE_ASSERT_ARGUMENT(out_device);
  *out_device = NULL;

  iree_hal_streaming_device_registry_t* device_registry =
      iree_hal_streaming_device_registry();
  if (!device_registry) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "HAL stream layer not initialized");
  }

  if (ordinal >= device_registry->device_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "device ordinal %zu out of range [0, %zu)", ordinal,
                            device_registry->device_count);
  }

  iree_hal_streaming_device_t* device = &device_registry->devices[ordinal];

  // Device is always created during initialization.
  // Primary context is created lazily on first access.
  IREE_ASSERT(device->hal_device);

  *out_device = device;

  return iree_ok_status();
}

iree_status_t iree_hal_streaming_device_name(
    iree_hal_streaming_device_ordinal_t ordinal, char* name,
    iree_host_size_t name_size) {
  IREE_ASSERT_ARGUMENT(name);
  if (name_size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "name_size must be > 0");
  }

  iree_hal_streaming_device_t* device = NULL;
  iree_status_t status = iree_hal_streaming_device_by_ordinal(ordinal, &device);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  // Calculate safe copy length: min(source_length, dest_size - 1)
  const iree_host_size_t source_len = device->info.name.size;
  const iree_host_size_t copy_len =
      source_len < (name_size - 1) ? source_len : (name_size - 1);

  // Copy the name data safely
  if (copy_len > 0) {
    memcpy(name, device->info.name.data, copy_len);
  }

  // Always null-terminate
  name[copy_len] = '\0';

  return iree_ok_status();
}

iree_status_t iree_hal_streaming_device_get_string_property(
    iree_hal_streaming_device_ordinal_t ordinal, const char* category,
    const char* key, char* value, iree_host_size_t value_size) {
  IREE_ASSERT_ARGUMENT(category);
  IREE_ASSERT_ARGUMENT(key);
  IREE_ASSERT_ARGUMENT(value);
  if (value_size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "value_size must be > 0");
  }

  iree_hal_streaming_device_t* device = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_streaming_device_by_ordinal(ordinal, &device));

  // The streaming layer owns the set of string-valued device properties and
  // resolves them from values cached on iree_hal_streaming_device_t during
  // device initialization. The underlying IREE HAL intentionally does not
  // expose a string-property query; callers must go through this API.
  iree_string_view_t source = iree_string_view_empty();
  if (iree_string_view_equal(iree_make_cstring_view(category),
                             IREE_SV("hal.device"))) {
    const iree_string_view_t key_sv = iree_make_cstring_view(key);
    if (iree_string_view_equal(key_sv, IREE_SV("name"))) {
      source = device->info.name;
    } else if (iree_string_view_equal(key_sv, IREE_SV("path"))) {
      source = device->info.path;
    } else if (iree_string_view_equal(key_sv, IREE_SV("architecture")) ||
               iree_string_view_equal(key_sv, IREE_SV("gcn_arch_name"))) {
      source = iree_make_cstring_view(device->gcn_arch_name);
    }
  }

  if (iree_string_view_is_empty(source)) {
    return iree_make_status(
        IREE_STATUS_NOT_FOUND,
        "unknown string device property '%s' in category '%s'", key, category);
  }

  if (source.size + 1 > value_size) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "buffer of %" PRIhsz
                            " bytes is too small for property '%s:%s' "
                            "(requires %" PRIhsz " bytes including NUL)",
                            value_size, category, key, source.size + 1);
  }
  memcpy(value, source.data, source.size);
  value[source.size] = '\0';
  return iree_ok_status();
}

iree_hal_streaming_p2p_link_t* iree_hal_streaming_device_lookup_p2p_link(
    iree_hal_streaming_device_ordinal_t src_device,
    iree_hal_streaming_device_ordinal_t dst_device) {
  iree_hal_streaming_device_registry_t* device_registry =
      iree_hal_streaming_device_registry();
  if (!device_registry || !device_registry->p2p_topology) {
    return NULL;
  }

  const iree_host_size_t device_count = device_registry->device_count;
  if (src_device >= device_count || dst_device >= device_count) {
    return NULL;
  }

  // Links are stored in row-major order: [src][dst].
  const iree_host_size_t link_index = src_device * device_count + dst_device;
  return &device_registry->p2p_topology[link_index];
}

iree_status_t iree_hal_streaming_device_memory_info(
    iree_hal_streaming_device_ordinal_t ordinal,
    iree_device_size_t* out_free_memory, iree_device_size_t* out_total_memory) {
  IREE_ASSERT_ARGUMENT(out_free_memory);
  IREE_ASSERT_ARGUMENT(out_total_memory);
  *out_free_memory = 0;
  *out_total_memory = 0;

  iree_hal_streaming_device_t* device = NULL;
  iree_status_t status = iree_hal_streaming_device_by_ordinal(ordinal, &device);
  if (iree_status_is_ok(status)) {
    *out_free_memory = (iree_device_size_t)iree_atomic_load(
        &device->free_memory, iree_memory_order_relaxed);
    *out_total_memory = device->total_memory;
  }
  return status;
}

iree_status_t iree_hal_streaming_device_can_access_peer(
    iree_hal_streaming_device_ordinal_t device_ordinal,
    iree_hal_streaming_device_ordinal_t peer_device_ordinal, bool* can_access) {
  IREE_ASSERT_ARGUMENT(can_access);
  IREE_TRACE_ZONE_BEGIN(z0);
  *can_access = false;

  iree_hal_streaming_device_registry_t* device_registry =
      iree_hal_streaming_device_registry();
  if (!device_registry) {
    IREE_RETURN_AND_END_ZONE_IF_ERROR(
        z0, iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                             "HAL stream layer not initialized"));
  }

  const iree_host_size_t device_count = device_registry->device_count;
  if (device_ordinal >= device_count || peer_device_ordinal >= device_count) {
    IREE_RETURN_AND_END_ZONE_IF_ERROR(
        z0, iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                             "device ordinals out of range"));
  }

  // Look up P2P link in topology.
  iree_hal_streaming_p2p_link_t* link =
      iree_hal_streaming_device_lookup_p2p_link(device_ordinal,
                                                peer_device_ordinal);
  if (!link) {
    *can_access = true;
  } else {
    *can_access = link->access_supported ? true : false;
  }

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

iree_status_t iree_hal_streaming_device_set_primary_context_flags(
    iree_hal_streaming_device_ordinal_t device_ordinal,
    const iree_hal_streaming_context_flags_t* flags) {
  IREE_ASSERT_ARGUMENT(flags);
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_streaming_device_t* device =
      iree_hal_streaming_device_entry(device_ordinal);
  if (!device) {
    IREE_RETURN_AND_END_ZONE_IF_ERROR(
        z0,
        iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                         "invalid device ordinal %" PRIhsz, device_ordinal));
  }

  iree_slim_mutex_lock(&device->primary_context_mutex);
  device->primary_context_flags = *flags;
  if (device->primary_context) {
    device->primary_context->flags = *flags;
  }
  iree_slim_mutex_unlock(&device->primary_context_mutex);

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

iree_status_t iree_hal_streaming_device_primary_context_state(
    iree_hal_streaming_device_ordinal_t device_ordinal,
    iree_hal_streaming_context_flags_t* out_flags, bool* out_active) {
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_streaming_device_t* device =
      iree_hal_streaming_device_entry(device_ordinal);
  if (!device) {
    IREE_RETURN_AND_END_ZONE_IF_ERROR(
        z0,
        iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                         "invalid device ordinal %" PRIhsz, device_ordinal));
  }

  iree_slim_mutex_lock(&device->primary_context_mutex);
  if (out_flags) {
    *out_flags = device->primary_context ? device->primary_context->flags
                                         : device->primary_context_flags;
  }
  if (out_active) {
    *out_active = device->primary_context != NULL;
  }
  iree_slim_mutex_unlock(&device->primary_context_mutex);

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static iree_status_t iree_hal_streaming_device_ensure_default_mem_pool_locked(
    iree_hal_streaming_device_t* device) {
  IREE_ASSERT_ARGUMENT(device);
  if (device->default_mem_pool && device->current_mem_pool) {
    return iree_ok_status();
  }

  if (!iree_hal_streaming_device_registry()) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "device registry not initialized");
  }

  hrx_mem_pool_t default_mem_pool = NULL;
  if (!device->default_mem_pool) {
    hrx_mem_pool_props_t props = {
        .alloc_handle_type = 0,
        .location_type = 1,  // device
        .location_id = (int)device->ordinal,
    };
    IREE_RETURN_IF_ERROR(HRX_CALL(
        hrx_mem_pool_create(device->hrx_device, &props, &default_mem_pool)));
    device->default_mem_pool = default_mem_pool;
  }

  if (!device->current_mem_pool) {
    device->current_mem_pool = device->default_mem_pool;
    hrx_mem_pool_retain(device->current_mem_pool);
  }
  return iree_ok_status();
}

iree_status_t iree_hal_streaming_device_ensure_default_mem_pool(
    iree_hal_streaming_device_t* device) {
  IREE_ASSERT_ARGUMENT(device);
  iree_slim_mutex_lock(&device->primary_context_mutex);
  iree_status_t status =
      iree_hal_streaming_device_ensure_default_mem_pool_locked(device);
  iree_slim_mutex_unlock(&device->primary_context_mutex);
  return status;
}

// Requires |device->primary_context_mutex| to be held. The context and its
// default allocation pool become visible together so callers never observe a
// context that cannot service runtime allocations.
static iree_status_t iree_hal_streaming_device_create_primary_context_locked(
    iree_hal_streaming_device_t* device) {
  IREE_ASSERT_ARGUMENT(device);
  if (device->primary_context) {
    return iree_hal_streaming_device_ensure_default_mem_pool_locked(device);
  }

  iree_hal_streaming_device_registry_t* device_registry =
      iree_hal_streaming_device_registry();
  if (!device_registry) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "device registry not initialized");
  }

  const bool had_default_mem_pool = device->default_mem_pool != NULL;
  const bool had_current_mem_pool = device->current_mem_pool != NULL;
  iree_hal_streaming_context_t* context = NULL;
  iree_status_t status = iree_hal_streaming_context_create(
      device, device->primary_context_flags, device_registry->host_allocator,
      &context);
  if (iree_status_is_ok(status)) {
    status = iree_hal_streaming_device_ensure_default_mem_pool_locked(device);
  }
  if (iree_status_is_ok(status)) {
    device->primary_context = context;
    return iree_ok_status();
  }

  iree_hal_streaming_context_release(context);
  if (!had_current_mem_pool) {
    hrx_mem_pool_release(device->current_mem_pool);
    device->current_mem_pool = NULL;
  }
  if (!had_default_mem_pool) {
    hrx_mem_pool_release(device->default_mem_pool);
    device->default_mem_pool = NULL;
  }
  return status;
}

iree_status_t iree_hal_streaming_device_get_or_create_primary_context(
    iree_hal_streaming_device_t* device,
    iree_hal_streaming_context_t** out_context) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_ASSERT_ARGUMENT(out_context);
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_context = NULL;

  iree_slim_mutex_lock(&device->primary_context_mutex);
  iree_status_t status =
      iree_hal_streaming_device_create_primary_context_locked(device);
  if (iree_status_is_ok(status)) {
    *out_context = device->primary_context;
  }
  iree_slim_mutex_unlock(&device->primary_context_mutex);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_hal_streaming_device_retain_primary_context(
    iree_hal_streaming_device_t* device,
    iree_hal_streaming_context_t** out_context) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_ASSERT_ARGUMENT(out_context);
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_context = NULL;

  iree_slim_mutex_lock(&device->primary_context_mutex);

  iree_status_t status = iree_ok_status();
  if (device->primary_context_ref_count == INT32_MAX) {
    status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "primary context reference count overflow");
  } else {
    ++device->primary_context_ref_count;
    status = iree_hal_streaming_device_create_primary_context_locked(device);
    if (iree_status_is_ok(status)) {
      iree_hal_streaming_context_retain(device->primary_context);
      *out_context = device->primary_context;
    } else {
      --device->primary_context_ref_count;
    }
  }

  iree_slim_mutex_unlock(&device->primary_context_mutex);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_hal_streaming_device_release_primary_context(
    iree_hal_streaming_device_t* device) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_slim_mutex_lock(&device->primary_context_mutex);

  // Check if context is retained.
  if (device->primary_context_ref_count == 0) {
    iree_slim_mutex_unlock(&device->primary_context_mutex);
    IREE_RETURN_AND_END_ZONE_IF_ERROR(
        z0, iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                             "primary context not retained"));
  }

  // Decrement reference count.
  device->primary_context_ref_count--;

  // If count reached 0, destroy the context.
  if (device->primary_context_ref_count == 0 && device->primary_context) {
    iree_hal_streaming_context_t* released_context = device->primary_context;

    // Wait for all operations to complete.
    iree_status_t status = iree_hal_streaming_context_wait_idle(
        released_context, iree_infinite_timeout());
    if (!iree_status_is_ok(status)) {
      iree_status_free(status);
    }

    // Clear current context if it was the primary context.
    iree_hal_streaming_context_t* current_context =
        iree_hal_streaming_context_current();
    if (current_context == released_context) {
      iree_hal_streaming_context_set_current(NULL);
    }

    // Release the context.
    iree_hal_streaming_context_release(released_context);
    device->primary_context = NULL;

    // Also clear memory pools.
    hrx_mem_pool_release(device->current_mem_pool);
    device->current_mem_pool = NULL;
    hrx_mem_pool_release(device->default_mem_pool);
    device->default_mem_pool = NULL;
  }

  iree_slim_mutex_unlock(&device->primary_context_mutex);
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Occupancy calculation helpers
//===----------------------------------------------------------------------===//

iree_status_t iree_hal_streaming_calculate_max_active_blocks_per_multiprocessor(
    iree_hal_streaming_device_t* device, iree_hal_streaming_symbol_t* symbol,
    uint32_t block_size, iree_device_size_t dynamic_shared_mem_size,
    uint32_t* out_max_blocks) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_ASSERT_ARGUMENT(symbol);
  IREE_ASSERT_ARGUMENT(out_max_blocks);

  // Verify the symbol is a function.
  if (symbol->type != IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "symbol is not a function (type=%d)", symbol->type);
  }

  if (block_size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "block size must be positive");
  }

  const uint32_t maximum_block_size =
      symbol->function_attributes.maximum_threads_per_block != 0
          ? iree_min(device->max_threads_per_block,
                     symbol->function_attributes.maximum_threads_per_block)
          : device->max_threads_per_block;
  if (block_size > maximum_block_size) {
    *out_max_blocks = 0;
    return iree_ok_status();
  }

  // Calculate constraints.
  // 1. Thread constraint: workgroups limited by resident invocations per
  // execution unit.
  const uint32_t blocks_by_threads =
      device->max_threads_per_multiprocessor / block_size;

  // 2. Workgroup constraint: hardware limit per execution unit.
  const uint32_t blocks_by_limit = device->max_blocks_per_multiprocessor;

  // 3. Register constraint: blocks limited by register usage.
  uint32_t blocks_by_regs = UINT32_MAX;
  const uint32_t register_count = symbol->function_attributes.register_count;
  if (register_count > 0) {
    if (IREE_UNLIKELY(device->warp_size == 0)) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "device reports a zero warp size");
    }
    // Round up register allocation to warp granularity.
    const uint32_t warps_per_block =
        block_size / device->warp_size + (block_size % device->warp_size != 0);
    const uint64_t registers_per_block =
        (uint64_t)register_count * warps_per_block * device->warp_size;
    if (registers_per_block > 0) {
      blocks_by_regs =
          registers_per_block > device->max_registers_per_multiprocessor
              ? 0
              : device->max_registers_per_multiprocessor /
                    registers_per_block;
    }
  }

  // 4. Shared memory constraint.
  uint32_t blocks_by_smem = UINT32_MAX;
  iree_device_size_t total_shared_memory = 0;
  if (IREE_UNLIKELY(!iree_device_size_checked_add(
          symbol->function_attributes.fixed_shared_memory_size,
          dynamic_shared_mem_size, &total_shared_memory))) {
    *out_max_blocks = 0;
    return iree_ok_status();
  }
  const iree_device_size_t maximum_dynamic_shared_memory =
      iree_all_bits_set(
          symbol->function_attributes.provided_flags,
          IREE_HAL_STREAMING_FUNCTION_ATTRIBUTE_FLAG_DYNAMIC_SHARED_MEMORY)
          ? iree_hal_streaming_function_attributes_dynamic_shared_memory_size(
                &symbol->function_attributes)
          : device->max_shared_memory_per_block;
  if (dynamic_shared_mem_size > maximum_dynamic_shared_memory ||
      total_shared_memory > device->max_shared_memory_per_block) {
    *out_max_blocks = 0;
    return iree_ok_status();
  }
  if (total_shared_memory > 0) {
    blocks_by_smem =
        total_shared_memory > device->max_shared_memory_per_multiprocessor
            ? 0
            : device->max_shared_memory_per_multiprocessor /
                  total_shared_memory;
  }

  // Take the minimum of all constraints.
  uint32_t max_blocks = blocks_by_threads;
  if (blocks_by_limit < max_blocks) max_blocks = blocks_by_limit;
  if (blocks_by_regs < max_blocks) max_blocks = blocks_by_regs;
  if (blocks_by_smem < max_blocks) max_blocks = blocks_by_smem;

  *out_max_blocks = max_blocks;
  return iree_ok_status();
}

iree_status_t iree_hal_streaming_calculate_optimal_block_size(
    iree_hal_streaming_device_t* device, iree_hal_streaming_symbol_t* symbol,
    iree_device_size_t dynamic_shared_mem_size,
    iree_hal_streaming_block_to_dynamic_smem_fn_t dynamic_shared_mem_callback,
    uint32_t block_size_limit, uint32_t* out_block_size,
    uint32_t* out_min_grid_size) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_ASSERT_ARGUMENT(symbol);
  IREE_ASSERT_ARGUMENT(out_block_size);
  IREE_ASSERT_ARGUMENT(out_min_grid_size);

  // Verify the symbol is a function.
  if (symbol->type != IREE_HAL_STREAMING_SYMBOL_TYPE_FUNCTION) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "symbol is not a function (type=%d)", symbol->type);
  }

  // Determine the maximum block size.
  uint32_t max_block_size =
      symbol->function_attributes.maximum_threads_per_block;
  if (max_block_size == 0) {
    max_block_size = device->max_threads_per_block;
  } else {
    max_block_size = iree_min(max_block_size, device->max_threads_per_block);
  }
  if (block_size_limit > 0 && block_size_limit < max_block_size) {
    max_block_size = block_size_limit;
  }
  if (max_block_size == 0) {
    *out_block_size = 0;
    *out_min_grid_size = 0;
    return iree_ok_status();
  }

  // Occupancy only changes at subgroup boundaries for thread residency. Walk
  // those boundaries from largest to smallest so equal-occupancy candidates
  // select the larger workgroup.
  const uint32_t block_size_step =
      device->warp_size != 0 ? device->warp_size : 1;
  uint32_t test_size = max_block_size >= block_size_step
                           ? max_block_size - max_block_size % block_size_step
                           : max_block_size;
  uint32_t best_block_size = 0;
  uint32_t best_occupancy = 0;
  while (test_size != 0) {
    // Calculate dynamic shared memory size for this block size.
    const iree_device_size_t dynamic_smem =
        dynamic_shared_mem_callback ? dynamic_shared_mem_callback(test_size)
                                    : dynamic_shared_mem_size;

    // Get max active blocks for this configuration.
    uint32_t active_blocks = 0;
    iree_status_t status =
        iree_hal_streaming_calculate_max_active_blocks_per_multiprocessor(
            device, symbol, test_size, dynamic_smem, &active_blocks);
    if (!iree_status_is_ok(status)) {
      return status;
    }

    if (IREE_UNLIKELY(active_blocks != 0 &&
                      test_size > UINT32_MAX / active_blocks)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "occupancy calculation overflow");
    }
    const uint32_t occupancy = active_blocks * test_size;

    // Update best if this is better.
    if (occupancy > best_occupancy) {
      best_occupancy = occupancy;
      best_block_size = test_size;
    }
    if (test_size <= block_size_step) break;
    test_size -= block_size_step;
  }

  if (best_block_size == 0) {
    *out_block_size = 0;
    *out_min_grid_size = 0;
    return iree_ok_status();
  }

  // Calculate grid size with the best block size.
  const uint32_t mp_count =
      device->multiprocessor_count > 0 ? device->multiprocessor_count : 1;

  // Get dynamic shared memory for the best block size.
  const iree_device_size_t best_dynamic_smem =
      dynamic_shared_mem_callback ? dynamic_shared_mem_callback(best_block_size)
                                  : dynamic_shared_mem_size;

  uint32_t blocks_per_mp = 0;
  IREE_RETURN_IF_ERROR(
      iree_hal_streaming_calculate_max_active_blocks_per_multiprocessor(
          device, symbol, best_block_size, best_dynamic_smem, &blocks_per_mp));

  if (IREE_UNLIKELY(blocks_per_mp != 0 &&
                    mp_count > UINT32_MAX / blocks_per_mp)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "occupancy grid size overflow");
  }
  const uint32_t minimum_grid_size = blocks_per_mp * mp_count;
  *out_block_size = best_block_size;
  *out_min_grid_size = minimum_grid_size;

  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Cooperative launch calculation helpers
//===----------------------------------------------------------------------===//

iree_status_t iree_hal_streaming_calculate_max_cooperative_blocks(
    iree_hal_streaming_device_t* device, iree_hal_streaming_symbol_t* symbol,
    uint32_t block_size, uint32_t dynamic_shared_mem_size,
    uint32_t* out_max_blocks) {
  IREE_ASSERT_ARGUMENT(device);
  IREE_ASSERT_ARGUMENT(symbol);
  IREE_ASSERT_ARGUMENT(out_max_blocks);

  // Check if device supports cooperative launch.
  // If not, return success with max blocks set to 0.
  if (!device->supports_cooperative_launch) {
    *out_max_blocks = 0;
    return iree_ok_status();
  }

  // For cooperative kernels, all blocks must be resident on the device at once.
  // Calculate the maximum number of active blocks per multiprocessor.
  uint32_t max_blocks_per_multiprocessor = 0;
  IREE_RETURN_IF_ERROR(
      iree_hal_streaming_calculate_max_active_blocks_per_multiprocessor(
          device, symbol, block_size, dynamic_shared_mem_size,
          &max_blocks_per_multiprocessor));

  if (IREE_UNLIKELY(max_blocks_per_multiprocessor != 0 &&
                    device->multiprocessor_count >
                        UINT32_MAX / max_blocks_per_multiprocessor)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "cooperative grid size overflow");
  }
  *out_max_blocks =
      max_blocks_per_multiprocessor * device->multiprocessor_count;

  return iree_ok_status();
}
