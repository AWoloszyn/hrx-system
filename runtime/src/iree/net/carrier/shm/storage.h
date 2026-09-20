// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Mapping and native wake ownership independent of a live connection/proactor.

#ifndef IREE_NET_CARRIER_SHM_STORAGE_H_
#define IREE_NET_CARRIER_SHM_STORAGE_H_

#include "iree/async/buffer_pool.h"
#include "iree/async/event.h"
#include "iree/base/internal/shm.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/carrier/shm/region.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// One mapping and two native events. Pipe-backed events transfer both ends so
// detached writers retain a read-end guard after the peer has departed.
#if defined(IREE_PLATFORM_WINDOWS) || defined(IREE_ASYNC_HAVE_EVENTFD)
#define IREE_NET_SHM_STORAGE_HANDLE_COUNT 3u
#else
#define IREE_NET_SHM_STORAGE_HANDLE_COUNT 5u
#endif

typedef struct iree_net_shm_storage_t iree_net_shm_storage_t;

// Receive lease context with one ownership counter per endpoint, not per slot.
typedef struct iree_net_shm_storage_endpoint_t {
  // Containing storage, retained independently by each native receive lease.
  iree_net_shm_storage_t* storage;
  // Incoming direction borrowed from the containing storage's view array.
  iree_net_shm_direction_t* incoming;
  // Outstanding native receive leases; at most slot_count - 1.
  iree_atomic_int32_t retained_count;
} iree_net_shm_storage_endpoint_t;

// Cold failure notification protected by the storage's failure mutex.
typedef struct iree_net_shm_storage_failure_callback_t {
  // Queues a poll-owner handoff with a connection drain hold. Called at most
  // once, under the failure mutex; cannot invoke user callbacks or reenter
  // storage. The connection detaches this callback before completing drain.
  void (*fn)(void* user_data);
  // Weak connection context, valid until detach returns.
  void* user_data;
} iree_net_shm_storage_failure_callback_t;

struct iree_net_shm_storage_t {
  // One reference for transport ownership and one per movable receive lease.
  iree_atomic_ref_count_t ref_count;
  // Allocator for this object and its trailing process-local view arrays.
  iree_allocator_t host_allocator;
  // Owned mapping; shared state survives native handle/peer closure.
  iree_shm_mapping_t mapping;
  // Owned native events, indexed by the polling side they wake.
  iree_async_event_native_t wakes[2];
  // Checked immutable geometry used by both mapped views.
  iree_net_shm_region_layout_t layout;
  // Local side: server 0, client 1; also the outgoing direction index.
  uint32_t side;
  // Direction views ordered [endpoint * 2 + sending_side].
  iree_net_shm_direction_t* directions;
  // Detached incoming lease contexts, one per endpoint.
  iree_net_shm_storage_endpoint_t* endpoints;
  // Cold error/detach synchronization; never acquired on successful returns.
  struct {
    // Protects status and the weak callback through handoff admission.
    iree_slim_mutex_t mutex;
    // First owned failure, retained even after connection detach.
    iree_status_t status;
    // Attached connection's normal terminal-failure handoff.
    iree_net_shm_storage_failure_callback_t callback;
  } failure;
};

// Creates the server's mapping and both directional wakes. No proactor is
// retained, and payload pages are left untouched until a send publishes them.
iree_status_t iree_net_shm_storage_create(
    const iree_net_shm_region_layout_t* layout, iree_allocator_t host_allocator,
    iree_net_shm_storage_t** out_storage);

// Imports a validated OFFER's complete, owned native resource bundle as the
// client side. Consumes and clears every handle on both success and failure.
// |layout| must already satisfy the client's configured resource limits.
iree_status_t iree_net_shm_storage_import(
    const iree_net_shm_region_layout_t* layout,
    iree_async_primitive_t handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT],
    iree_allocator_t host_allocator, iree_net_shm_storage_t** out_storage);

// Exports borrowed local handles for OFFER. The storage owner must remain live
// through ACCEPT, not merely the local stream's send completion.
void iree_net_shm_storage_export(
    iree_net_shm_storage_t* storage,
    iree_async_primitive_t out_handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT]);

void iree_net_shm_storage_retain(iree_net_shm_storage_t* storage);
void iree_net_shm_storage_release(iree_net_shm_storage_t* storage);

// Installs the connection's weak error handoff before exposing receive leases.
// A stored failure immediately admits that handoff. Passing a zero callback
// detaches it and joins any concurrent admission before returning.
void iree_net_shm_storage_set_failure_callback(
    iree_net_shm_storage_t* storage,
    iree_net_shm_storage_failure_callback_t callback);

// Stores an owned terminal failure and schedules the attached connection once.
// Detached storage retains the first status until destruction.
void iree_net_shm_storage_fail(iree_net_shm_storage_t* storage,
                               iree_status_t status);

// Returns an owned copy of the stored terminal failure, or OK before failure.
iree_status_t iree_net_shm_storage_clone_failure(
    iree_net_shm_storage_t* storage);

// Publishes an epoch and wakes |side|'s polling owner. Called once per progress
// batch; detached lease returns call this without a managed notification.
iree_status_t iree_net_shm_storage_signal(iree_net_shm_storage_t* storage,
                                          uint32_t side);

// Attempts to lease an already consumed incoming slot. Only the receiving poll
// owner calls this. Returns false (and an empty lease) at the N-1 retention
// limit; the caller then delivers borrowed bytes and returns the slot directly.
bool iree_net_shm_storage_try_lease(iree_net_shm_storage_endpoint_t* endpoint,
                                    uint16_t slot, uint32_t length,
                                    iree_async_buffer_lease_t* out_lease);

// Returns an unmoved native lease on the receiving poll owner without
// signaling. The carrier includes this return in its batch wake. |lease| must
// be a live lease from try_lease, not a lease moved to application ownership.
void iree_net_shm_storage_recycle_lease(iree_async_buffer_lease_t* lease);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_STORAGE_H_
