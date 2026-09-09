// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_MEMORY_H_
#define AMDF_SRC_MEMORY_H_

#include "amdf/amdf.h"
#include "libamdf/src/child_tracker.h"
#include "libamdf/src/memory_pair.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct amdf_memory_vtable_t {
  // Exports one logical range as a complete owned transport value. Failure
  // releases every partial resource and leaves the result storage empty.
  amdf_status_t (*export_external)(amdf_memory_t* memory,
                                   const amdf_memory_export_info_t* export_info,
                                   amdf_external_memory_t* out_value);
  // Copies exact directional facts into caller-private result storage.
  amdf_status_t (*query_pair_info)(amdf_memory_t* producer_memory,
                                   const amdf_memory_pair_query_t* query,
                                   amdf_memory_pair_info_t* out_info);
  // Creates one explicit host mapping. Failure releases every partial resource;
  // success returns one complete mapping.
  amdf_status_t (*map)(amdf_memory_t* memory,
                       const amdf_memory_profile_t* profile,
                       const amdf_memory_map_info_t* map_info,
                       amdf_host_mapping_t** out_mapping);
  // Releases the exact native state owned by a memory implementation.
  amdf_status_t (*destroy_native)(amdf_memory_t* memory);
} amdf_memory_vtable_t;

struct amdf_memory_t {
  // Host allocator copied for direct terminal teardown.
  amdf_allocator_t host_allocator;
  // Implementation operations selected before the memory is published.
  const amdf_memory_vtable_t* vtable;
  // Device borrowed for the lifetime of this memory attachment.
  amdf_device_t* device;
  // Immutable properties established before publication.
  amdf_memory_info_t info;
  // Number of live mappings and commands borrowing this memory.
  amdf_child_tracker_t children;
};

// Initializes an unpublished memory base and borrows its device.
amdf_status_t amdf_memory_initialize(amdf_memory_t* memory,
                                     const amdf_memory_vtable_t* vtable,
                                     amdf_device_t* device);

// Releases the device borrow held by unpublished or torn-down memory.
void amdf_memory_deinitialize(amdf_memory_t* memory);

// Registers one child that borrows `memory`.
amdf_status_t amdf_memory_register_child(amdf_memory_t* memory);

// Releases one child borrow.
void amdf_memory_unregister_child(amdf_memory_t* memory);

// Returns the host allocator copied by the memory attachment.
amdf_allocator_t amdf_memory_host_allocator(const amdf_memory_t* memory);

// Creates memory attached to `device`.
amdf_status_t AMDF_CALL amdf_memory_create(
    amdf_device_t* device, const amdf_memory_create_info_t* create_info,
    amdf_memory_t** out_memory);

// Copies one immutable memory profile supported by `device`.
amdf_status_t AMDF_CALL amdf_device_query_memory_profile(
    amdf_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile);

// Imports external memory as a ready attachment to `device`.
amdf_status_t AMDF_CALL amdf_memory_import(
    amdf_device_t* device, const amdf_memory_import_info_t* import_info,
    amdf_external_memory_t* inout_external_memory, amdf_memory_t** out_memory);

// Copies immutable memory properties.
amdf_status_t AMDF_CALL amdf_memory_query_info(amdf_memory_t* memory,
                                               amdf_memory_info_t* out_info);

// Exports one logical range as a move-owned external value.
amdf_status_t AMDF_CALL amdf_memory_export(
    amdf_memory_t* memory, const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value);

// Releases and zeros one move-owned external value.
void AMDF_CALL amdf_external_memory_release(amdf_external_memory_t* value);

// Copies exact directional facts for two concrete attachment sites.
amdf_status_t AMDF_CALL amdf_memory_query_pair_info(
    const amdf_memory_site_t* producer_site,
    const amdf_memory_site_t* consumer_site, amdf_memory_pair_info_t* out_info);

// Creates an explicit host mapping of one memory range.
amdf_status_t AMDF_CALL amdf_memory_map(amdf_memory_t* memory,
                                        const amdf_memory_map_info_t* map_info,
                                        amdf_host_mapping_t** out_mapping);

// Destroys memory with no remaining children.
amdf_status_t AMDF_CALL amdf_memory_destroy(amdf_memory_t* memory);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_MEMORY_H_
