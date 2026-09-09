// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_QUEUE_H_
#define AMDF_QUEUE_H_

#include "amdf/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Native command representation accepted by a queue family.
typedef uint32_t amdf_queue_command_type_t;
enum amdf_queue_command_type_e {
  /// No command representation. Advertised families never use this value.
  AMDF_QUEUE_COMMAND_TYPE_UNKNOWN = 0,
  /// Native AMD GPU PM4 command streams.
  AMDF_QUEUE_COMMAND_TYPE_GPU_PM4 = 1,
  /// Native AMD GPU SDMA command streams.
  AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA = 2,
  /// Native AMD GPU AQL packets reaching hardware without CPU translation.
  AMDF_QUEUE_COMMAND_TYPE_GPU_AQL = 3,
  /// Native AMD XDNA execution commands.
  AMDF_QUEUE_COMMAND_TYPE_XDNA = 4,
};

/// Queue publication mechanisms implemented by a provider.
typedef uint32_t amdf_queue_publication_modes_t;
enum amdf_queue_publication_mode_bits_e {
  /// Commands are published directly through caller-mapped queue state.
  AMDF_QUEUE_PUBLICATION_MODE_USER = 1u << 0,
  /// Commands are accepted through a bounded provider call.
  AMDF_QUEUE_PUBLICATION_MODE_KERNEL = 1u << 1,
};

/// Immutable properties of one endpoint-local native queue family.
///
/// A family identifies one command representation independently from the
/// mechanisms available to publish it. Advertising a publication mode is a
/// promise that a later queue constructor can create that command/publication
/// pair for the opened endpoint; it is not a list of theoretical hardware
/// capabilities.
typedef struct amdf_queue_family_info_t {
  /// Must be `AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_queue_family_info_t)`.
  uint32_t structure_size;
  /// Optional output extension chain. No extensions are defined in ABI v1.
  void* next;
  /// Dense ordinal accepted by later queue creation operations.
  uint32_t ordinal;
  /// Native command representation accepted by queues in this family.
  amdf_queue_command_type_t command_type;
  /// User- and kernel-mode publication paths implemented by the provider.
  amdf_queue_publication_modes_t publication_modes;
} amdf_queue_family_info_t;

/// An infinite timeout accepted by operations that explicitly wait.
#define AMDF_TIMEOUT_INFINITE UINT64_MAX

/// Lifecycle state of one kernel-mediated queue.
typedef uint32_t amdf_kernel_queue_state_t;
enum amdf_kernel_queue_state_e {
  /// The queue accepts new work subject to its reported capacity.
  AMDF_KERNEL_QUEUE_STATE_ACTIVE = 1,
  /// The queue encountered a terminal provider or firmware failure.
  AMDF_KERNEL_QUEUE_STATE_FAILED = 2,
  /// The queue's device can no longer execute or retire work.
  AMDF_KERNEL_QUEUE_STATE_DEVICE_LOST = 3,
};

/// Immutable properties of one kernel-mediated queue.
typedef struct amdf_kernel_queue_info_t {
  /// Must be `AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_INFO`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_kernel_queue_info_t)`.
  uint32_t structure_size;
  /// Optional output extension chain. No extensions are currently defined.
  void* next;
  /// Identity of the materialized device owning this queue.
  amdf_device_id_t device_id;
  /// Device reset epoch in which this queue remains valid.
  uint64_t reset_epoch;
  /// Endpoint-local family selected when the queue was created.
  uint32_t queue_family_ordinal;
  /// Native command representation accepted by the queue.
  amdf_queue_command_type_t command_type;
  /// Maximum accepted submissions that may remain unretired.
  uint32_t maximum_pending_submission_count;
  /// Maximum commands accepted by one submission.
  uint32_t maximum_command_count;
} amdf_kernel_queue_info_t;

/// Current retirement and terminal state of one kernel-mediated queue.
typedef struct amdf_kernel_queue_status_t {
  /// Must be `AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_STATUS`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_kernel_queue_status_t)`.
  uint32_t structure_size;
  /// Optional output extension chain. No extensions are currently defined.
  void* next;
  /// Greatest accepted submission whose native storage is no longer in use.
  uint64_t retired_submission;
  /// Current queue lifecycle state.
  amdf_kernel_queue_state_t state;
  /// Reserved for future use and always zero.
  uint32_t reserved;
  /// Sticky terminal failure, or `AMDF_STATUS_OK` while active.
  amdf_status_t terminal_status;
} amdf_kernel_queue_status_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_QUEUE_H_
