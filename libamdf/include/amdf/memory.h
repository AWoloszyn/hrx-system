// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_MEMORY_H_
#define AMDF_MEMORY_H_

#include "amdf/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Physical placement class requested for a memory allocation.
typedef uint32_t amdf_memory_class_t;
enum amdf_memory_class_e {
  /// No placement class. This value is never accepted by memory creation.
  AMDF_MEMORY_CLASS_UNKNOWN = 0,
  /// Provider-owned system memory accessible through a host mapping.
  AMDF_MEMORY_CLASS_SYSTEM = 1,
  /// Device-local physical memory that may not be host visible.
  AMDF_MEMORY_CLASS_LOCAL = 2,
  /// Caller-owned host memory registered with a device.
  AMDF_MEMORY_CLASS_REGISTERED_HOST = 3,
};

/// Required or achieved properties of a memory attachment.
typedef uint64_t amdf_memory_flags_t;
enum amdf_memory_flag_bits_e {
  /// The allocation can be explicitly mapped for host access.
  AMDF_MEMORY_FLAG_HOST_VISIBLE = UINT64_C(1) << 0,
  /// The physical placement is local to the attached device.
  AMDF_MEMORY_FLAG_DEVICE_LOCAL = UINT64_C(1) << 1,
  /// The physical backing can be exported and attached to another device.
  AMDF_MEMORY_FLAG_SHAREABLE = UINT64_C(1) << 2,
  /// The allocation can hold instructions executable by the device.
  AMDF_MEMORY_FLAG_EXECUTABLE = UINT64_C(1) << 3,
  /// The allocation can hold directly published user-mode queue state.
  AMDF_MEMORY_FLAG_QUEUE_STORAGE = UINT64_C(1) << 4,
  /// Host and device access requires no explicit host cache transition.
  AMDF_MEMORY_FLAG_HOST_COHERENT = UINT64_C(1) << 5,
  /// A stable device address is established before creation returns.
  AMDF_MEMORY_FLAG_DEVICE_ADDRESS = UINT64_C(1) << 6,
};

/// Parameters used to create physical backing and attach it to one device.
typedef struct amdf_memory_create_info_t {
  /// Must be `AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_memory_create_info_t)`.
  uint32_t structure_size;
  /// Optional input extension chain. No extensions are currently defined.
  const void* next;
  /// Required physical placement class.
  amdf_memory_class_t memory_class;
  /// Required properties that must all be achieved.
  amdf_memory_flags_t required_flags;
  /// Minimum usable byte length. The achieved allocation may be larger.
  uint64_t byte_length;
  /// Minimum power-of-two allocation-base alignment in every supported address
  /// space, or zero for provider policy.
  uint64_t minimum_alignment;
  /// Borrowed host base for `AMDF_MEMORY_CLASS_REGISTERED_HOST`, otherwise
  /// `NULL`. The caller keeps this address range backed by the same live pages
  /// until `memory_destroy` succeeds. Registration does not take ownership.
  void* registered_host_pointer;
} amdf_memory_create_info_t;

/// Immutable properties of one live device memory attachment.
typedef struct amdf_memory_info_t {
  /// Must be `AMDF_STRUCTURE_TYPE_MEMORY_INFO`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_memory_info_t)`.
  uint32_t structure_size;
  /// Optional output extension chain. No extensions are currently defined.
  void* next;
  /// Achieved physical placement class.
  amdf_memory_class_t memory_class;
  /// Achieved attachment properties.
  amdf_memory_flags_t flags;
  /// Physical allocation length in bytes.
  uint64_t byte_length;
  /// Guaranteed power-of-two allocation-base alignment in every supported
  /// address space.
  uint64_t alignment;
  /// Identity shared by attachments to the same physical backing, when known.
  amdf_physical_memory_id_t physical_backing_id;
  /// Stable device virtual base when `AMDF_MEMORY_FLAG_DEVICE_ADDRESS` is set.
  uint64_t device_address;
  /// Device reset epoch in which the attachment and address remain valid.
  uint64_t reset_epoch;
} amdf_memory_info_t;

/// Host access requested for one explicit mapping.
typedef uint32_t amdf_memory_map_flags_t;
enum amdf_memory_map_flag_bits_e {
  /// Host loads are permitted from the mapped range.
  AMDF_MEMORY_MAP_FLAG_READ = 1u << 0,
  /// Host stores are permitted to the mapped range.
  AMDF_MEMORY_MAP_FLAG_WRITE = 1u << 1,
};

/// Parameters used to map a range of host-visible memory.
typedef struct amdf_memory_map_info_t {
  /// Must be `AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_memory_map_info_t)`.
  uint32_t structure_size;
  /// Optional input extension chain. No extensions are currently defined.
  const void* next;
  /// Byte offset into the physical allocation.
  uint64_t byte_offset;
  /// Nonzero byte length of the mapped range.
  uint64_t byte_length;
  /// Required host read and write access.
  amdf_memory_map_flags_t flags;
} amdf_memory_map_info_t;

/// Host cache behavior of a mapped allocation.
typedef uint32_t amdf_host_cacheability_t;
enum amdf_host_cacheability_e {
  /// The provider cannot describe the mapping's cache behavior.
  AMDF_HOST_CACHEABILITY_UNKNOWN = 0,
  /// Host and device accesses are mutually coherent without cache control.
  AMDF_HOST_CACHEABILITY_COHERENT = 1,
  /// Ordinary host write-back caching requiring explicit ownership transfer.
  AMDF_HOST_CACHEABILITY_WRITE_BACK = 2,
  /// Host write-combined caching intended for sequential stores.
  AMDF_HOST_CACHEABILITY_WRITE_COMBINED = 3,
  /// Uncached host access.
  AMDF_HOST_CACHEABILITY_UNCACHED = 4,
};

/// Immutable properties of one live host mapping.
typedef struct amdf_host_mapping_info_t {
  /// Must be `AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO`.
  amdf_structure_type_t type;
  /// Must be at least `sizeof(amdf_host_mapping_info_t)`.
  uint32_t structure_size;
  /// Optional output extension chain. No extensions are currently defined.
  void* next;
  /// Achieved host read and write access.
  amdf_memory_map_flags_t flags;
  /// Host cache behavior of the mapped pages.
  amdf_host_cacheability_t cacheability;
  /// First mapped byte borrowed until `host_mapping_destroy` succeeds.
  void* pointer;
  /// Mapped byte length.
  uint64_t byte_length;
  /// Host cache-line length in bytes, or zero when not applicable.
  uint32_t cache_line_size;
  /// Device reset epoch in which the mapping remains valid.
  uint64_t reset_epoch;
} amdf_host_mapping_info_t;

/// Direction of one explicit host cache ownership transition.
typedef uint32_t amdf_host_cache_operation_t;
enum amdf_host_cache_operation_e {
  /// Releases prior host writes for subsequent device reads.
  AMDF_HOST_CACHE_OPERATION_FLUSH = 1,
  /// Acquires prior device writes for subsequent host reads.
  AMDF_HOST_CACHE_OPERATION_INVALIDATE = 2,
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_MEMORY_H_
