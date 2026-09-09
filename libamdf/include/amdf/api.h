// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_API_H_
#define AMDF_API_H_

#include "amdf/base.h"
#include "amdf/memory.h"
#include "amdf/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Immutable entry-point table for one negotiated ABI version.
///
/// Tables grow only by appending fields. The table and every function pointer
/// reachable from it remain valid until the providing library is unloaded.
typedef struct amdf_api_t {
  /// Size in bytes of this table version.
  uint32_t structure_size;
  /// ABI version implemented by this table.
  amdf_abi_version_t abi_version;

  /// Creates an independent provider instance.
  ///
  /// The instance owns every dependent library reference and resolved native
  /// procedure table used by its children. Creation is thread-safe and performs
  /// bounded constant work. It performs no endpoint enumeration, device or
  /// firmware initialization, worker creation, retry, sleep, or process-global
  /// initialization. Failure leaves `out_instance` unchanged.
  amdf_status_t(AMDF_CALL* instance_create)(
      const amdf_instance_create_info_t* create_info,
      amdf_instance_t** out_instance);

  /// Destroys an instance after all of its children have been closed.
  ///
  /// The caller must have exclusive access. Returns
  /// `AMDF_STATUS_CODE_BUSY` without mutation while a child remains open.
  /// Destruction performs no implicit device wait.
  amdf_status_t(AMDF_CALL* instance_destroy)(amdf_instance_t* instance);

  /// Enumerates a bounded snapshot of independently selectable AMD endpoints.
  ///
  /// A zero `capacity` queries the total count and may pass `summaries` as
  /// `NULL`. A nonzero capacity requires `summaries` to reference that many
  /// elements. When capacity is insufficient, the available prefix is written,
  /// `out_count` receives the total, and `AMDF_STATUS_CODE_BUFFER_TOO_SMALL` is
  /// returned. Every other failure leaves both outputs unchanged. The call
  /// creates no device, paging queue, address space, context, allocation,
  /// executable, or hardware queue. Arrival or removal may change the result of
  /// a later call.
  amdf_status_t(AMDF_CALL* endpoint_enumerate)(
      amdf_instance_t* instance, uint32_t capacity,
      amdf_endpoint_summary_t* summaries, uint32_t* out_count);

  /// Opens one endpoint identity without enumerating the machine again.
  ///
  /// The returned query-only endpoint borrows `instance`; the instance must
  /// outlive it. Opening may acquire a native query handle and cache immutable
  /// identity, but creates no schedulable device state. A stale identity fails
  /// rather than selecting another endpoint. Failure leaves `out_endpoint`
  /// unchanged.
  amdf_status_t(AMDF_CALL* endpoint_open)(amdf_instance_t* instance,
                                          const amdf_endpoint_id_t* id,
                                          amdf_endpoint_t** out_endpoint);

  /// Copies immutable cached properties without a system call or device wait.
  ///
  /// The operation is thread-safe. The caller initializes `out_info` and its
  /// complete extension chain before the call. No output is modified when
  /// validation fails.
  amdf_status_t(AMDF_CALL* endpoint_query_info)(amdf_endpoint_t* endpoint,
                                                amdf_endpoint_info_t* out_info);

  /// Closes a query-only endpoint after all future children are destroyed.
  ///
  /// The caller must have exclusive access. The operation performs no implicit
  /// device wait. Failure leaves the endpoint live so destruction can be
  /// retried.
  amdf_status_t(AMDF_CALL* endpoint_close)(amdf_endpoint_t* endpoint);

  /// Acquires an immutable optional API table compiled into this library.
  ///
  /// Extension availability describes the library composition and never
  /// depends on endpoint enumeration or active hardware. Hardware support is
  /// reported by operations on the returned table. `minimum_version` and
  /// `maximum_version` form an inclusive range. An unknown or excluded
  /// extension returns `AMDF_STATUS_CODE_UNSUPPORTED`; a compiled extension
  /// with no version in range returns `AMDF_STATUS_CODE_VERSION_MISMATCH`.
  /// Failure leaves `out_extension_api` unchanged when it is non-NULL.
  ///
  /// This operation is thread-safe, bounded constant time, and inert. It
  /// performs no allocation, system call, device discovery, dependent-library
  /// load, or other observable initialization. The returned table remains
  /// valid until the providing library is unloaded.
  amdf_status_t(AMDF_CALL* query_extension)(amdf_extension_id_t extension_id,
                                            uint32_t minimum_version,
                                            uint32_t maximum_version,
                                            const void** out_extension_api);

  /// Copies one immutable queue-family record cached while opening `endpoint`.
  ///
  /// `queue_family_ordinal` must be less than the endpoint's reported family
  /// count. The operation is thread-safe and performs no system call,
  /// allocation, device initialization, queue creation, retry, sleep, or
  /// device wait. The caller initializes `out_info` and its complete extension
  /// chain. No output is modified on failure.
  amdf_status_t(AMDF_CALL* endpoint_query_queue_family_info)(
      amdf_endpoint_t* endpoint, uint32_t queue_family_ordinal,
      amdf_queue_family_info_t* out_info);

  /// Destroys a materialized device after all of its children are destroyed.
  ///
  /// The caller must have exclusive access. Returns
  /// `AMDF_STATUS_CODE_BUSY` without native mutation while a child remains
  /// live. Caller-submitted work must already be retired before its owning
  /// children are destroyed. A native teardown failure leaves the device live
  /// so destruction can be retried.
  amdf_status_t(AMDF_CALL* device_destroy)(amdf_device_t* device);

  /// Creates physical backing and one stable attachment to `device`.
  ///
  /// The returned memory borrows `device`, which must outlive it. Every bit in
  /// `required_flags` is guaranteed in the copied memory info. In particular,
  /// `AMDF_MEMORY_FLAG_DEVICE_ADDRESS` means that all ordinary mapping and
  /// residency work has completed and the address is ready for any supported
  /// consumer when this cold call returns. This operation performs no queue
  /// submission, command inspection, retry, or device-wide synchronization.
  /// Registered host memory borrows the supplied pages without copying their
  /// contents. If the pointer came from a host mapping, that source mapping
  /// and its memory must outlive the registration. Independent registrations
  /// do not transfer ownership or establish execution or cache dependencies.
  /// Failure leaves `out_memory` unchanged.
  amdf_status_t(AMDF_CALL* memory_create)(
      amdf_device_t* device, const amdf_memory_create_info_t* create_info,
      amdf_memory_t** out_memory);

  /// Copies immutable properties cached when `memory` was created.
  ///
  /// The operation is thread-safe and performs no system call, allocation,
  /// mapping mutation, retry, sleep, or device wait. The caller initializes
  /// `out_info` and its complete extension chain. No output is modified when
  /// validation fails.
  amdf_status_t(AMDF_CALL* memory_query_info)(amdf_memory_t* memory,
                                              amdf_memory_info_t* out_info);

  /// Creates an explicit host mapping of one memory range.
  ///
  /// The returned mapping borrows `memory`, which must outlive it. Mapping does
  /// not wait for device work or transfer cache ownership. Failure leaves
  /// `out_mapping` unchanged.
  amdf_status_t(AMDF_CALL* memory_map)(amdf_memory_t* memory,
                                       const amdf_memory_map_info_t* map_info,
                                       amdf_host_mapping_t** out_mapping);

  /// Copies immutable properties of one live host mapping.
  ///
  /// The copied pointer is borrowed until `host_mapping_destroy` succeeds. The
  /// operation is thread-safe and performs no system call, allocation, cache
  /// transition, or device wait. No output is modified on validation failure.
  amdf_status_t(AMDF_CALL* host_mapping_query_info)(
      amdf_host_mapping_t* mapping, amdf_host_mapping_info_t* out_info);

  /// Performs one explicit host cache ownership transition over a mapped range.
  ///
  /// `byte_offset` is relative to the mapping. The implementation may touch
  /// every cache line intersecting the range; callers externally synchronize
  /// the complete intersected lines. A non-empty flush requires write access
  /// and a non-empty invalidate requires read access; otherwise the operation
  /// returns `AMDF_STATUS_CODE_FAILED_PRECONDITION`. An empty range is a no-op.
  /// This operation never waits for device execution or supplies an execution
  /// dependency.
  amdf_status_t(AMDF_CALL* host_mapping_cache_control)(
      amdf_host_mapping_t* mapping, amdf_host_cache_operation_t operation,
      uint64_t byte_offset, uint64_t byte_length);

  /// Destroys one mapping after all host access to its pointer has stopped.
  ///
  /// The caller must have exclusive access. Failure leaves the mapping live so
  /// destruction can be retried. No device wait or cache transition is implied.
  amdf_status_t(AMDF_CALL* host_mapping_destroy)(amdf_host_mapping_t* mapping);

  /// Destroys memory after all host mappings and future device uses are gone.
  ///
  /// The caller must have exclusive access. Returns `AMDF_STATUS_CODE_BUSY`
  /// without native mutation while a mapping remains live. Destruction performs
  /// no implicit device wait or cache transition. A native teardown failure
  /// leaves the memory live so destruction can be retried.
  amdf_status_t(AMDF_CALL* memory_destroy)(amdf_memory_t* memory);

  /// Copies immutable properties cached when `queue` was created.
  ///
  /// The operation is thread-safe and performs no system call, allocation,
  /// native progress query, retry, sleep, or device wait. No output is modified
  /// when validation fails.
  amdf_status_t(AMDF_CALL* kernel_queue_query_info)(
      amdf_kernel_queue_t* queue, amdf_kernel_queue_info_t* out_info);

  /// Samples retirement and observed terminal state without waiting.
  ///
  /// The operation may retire completed submissions and release their command
  /// borrows. It is thread-safe with submission and other status operations. It
  /// performs no allocation, system call, retry, sleep, or active polling. No
  /// output is modified when validation fails. ACTIVE means no terminal failure
  /// has been observed, not that a fresh native health check was performed.
  /// Rejection and timeout errors do not themselves mark a queue failed. A
  /// terminal failure remains sticky and is not itself retirement proof.
  /// Providers without a mapped completion fence report cached progress;
  /// `kernel_queue_wait`, including a zero-time wait, refreshes that progress.
  amdf_status_t(AMDF_CALL* kernel_queue_query_status)(
      amdf_kernel_queue_t* queue, amdf_kernel_queue_status_t* out_status);

  /// Waits until `submission` retires, a failure is observed, or time expires.
  ///
  /// `timeout_nanoseconds` includes host contention, active polling, and native
  /// waiting under one deadline. A zero timeout performs one nonblocking native
  /// poll when progress is not already known. `poll_duration_nanoseconds` is
  /// clipped to that timeout; zero disables active polling.
  /// `AMDF_TIMEOUT_INFINITE` requests no deadline. A
  /// timeout observes but never cancels accepted work or releases its command
  /// borrows. The operation is thread-safe with submission and status queries.
  /// A native wait error is returned even if progress concurrently advances;
  /// callers use `kernel_queue_query_status` to determine retirement and
  /// whether a terminal failure was observed before deciding to retry.
  amdf_status_t(AMDF_CALL* kernel_queue_wait)(
      amdf_kernel_queue_t* queue, uint64_t submission,
      uint64_t timeout_nanoseconds, uint64_t poll_duration_nanoseconds);

  /// Destroys one queue after every accepted submission has retired.
  ///
  /// The caller must have exclusive access. The operation samples progress once
  /// and returns `AMDF_STATUS_CODE_BUSY` without waiting while work remains. A
  /// native teardown failure leaves the queue live so destruction can be
  /// retried.
  amdf_status_t(AMDF_CALL* kernel_queue_destroy)(amdf_kernel_queue_t* queue);
} amdf_api_t;

/// Function type used to acquire the immutable API table.
typedef amdf_status_t(AMDF_CALL* amdf_query_api_fn_t)(
    amdf_abi_version_t minimum_version, amdf_abi_version_t maximum_version,
    const amdf_api_t** out_api);

/// Acquires the newest supported API table in the inclusive requested range.
///
/// On success, `out_api` receives a borrowed immutable table that remains valid
/// until the providing library is unloaded. Failure leaves `out_api` unchanged
/// when it is non-NULL.
///
/// This function is thread-safe, bounded constant time, and inert. It performs
/// no allocation, system call, device discovery, dependent-library load, or
/// other observable initialization.
AMDF_API amdf_status_t AMDF_CALL
amdf_query_api(amdf_abi_version_t minimum_version,
               amdf_abi_version_t maximum_version, const amdf_api_t** out_api);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_API_H_
