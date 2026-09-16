// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Sparse slot allocator for io_uring resource tables.
//
// io_uring supports sparse resource tables (kernel 5.19+) where an empty table
// of a fixed capacity is pre-registered, then individual slots are
// populated/cleared dynamically. This applies to both the fixed buffer table
// (IORING_REGISTER_BUFFERS2 + IORING_REGISTER_BUFFERS_UPDATE) and the fixed
// file table (IORING_REGISTER_FILES2 + IORING_REGISTER_FILES_UPDATE2).
//
// This type manages only the bitmap allocation of slots. Kernel syscalls are
// the caller's responsibility, since the register opcode and data format differ
// between buffers (iovec) and files (int fd). The intended usage pattern is:
//
//   int32_t slot = iree_io_uring_sparse_table_acquire(table, count);
//   if (slot < 0) return RESOURCE_EXHAUSTED;
//   int ret = iree_io_uring_ring_register(...);
//   if (ret < 0) { iree_io_uring_sparse_table_release(table, slot, count); }
//
// Reserved slots remain unavailable until explicitly released. Callers must
// not hold table synchronization while executing a kernel registration: on a
// SINGLE_ISSUER ring that operation may wait for the poll owner.

#ifndef IREE_ASYNC_PLATFORM_IO_URING_SPARSE_TABLE_H_
#define IREE_ASYNC_PLATFORM_IO_URING_SPARSE_TABLE_H_

#include "iree/base/api.h"
#include "iree/base/threading/mutex.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Default sparse buffer table capacity (slot count). Bitmap cost is 1KB at
// 8192 slots. A 512-buffer slab consumes 512 contiguous slots, so this
// supports ~16 concurrent slab registrations plus individual dmabuf/file slots
// without fragmentation pressure.
#define IREE_IO_URING_SPARSE_TABLE_DEFAULT_CAPACITY 8192

// A sparse slot allocator for io_uring resource tables.
//
// Wraps an iree_bitmap_t with mutex protection and first-fit contiguous
// allocation semantics. Acquire and release are independently thread-safe.
//
// Used for both IORING_REGISTER_BUFFERS2 (fixed buffer table) and
// IORING_REGISTER_FILES2 (fixed file table) sparse registrations.
typedef struct iree_io_uring_sparse_table_t {
  // Serializes bitmap reservation and release.
  iree_slim_mutex_t mutex;

  // Bitmap of allocated slots. bit_count == table capacity.
  // The words array are allocated as trailing data after this struct.
  iree_bitmap_t bitmap;
} iree_io_uring_sparse_table_t;

// Allocates a sparse table with the given slot |capacity|.
// Single allocation: struct + trailing bitmap words. All slots start free.
iree_status_t iree_io_uring_sparse_table_allocate(
    uint16_t capacity, iree_allocator_t allocator,
    iree_io_uring_sparse_table_t** out_table);

// Frees the sparse table. No-op if |table| is NULL.
// The caller must ensure no kernel resource table references remain.
void iree_io_uring_sparse_table_free(iree_io_uring_sparse_table_t* table,
                                     iree_allocator_t allocator);

// Acquires a contiguous range of |count| slots using first-fit strategy.
// Returns the starting index, or -1 if insufficient contiguous space. The
// slots remain reserved until a matching release call.
int32_t iree_io_uring_sparse_table_acquire(iree_io_uring_sparse_table_t* table,
                                           uint16_t count);

// Releases a contiguous range of |count| slots starting at |start|.
void iree_io_uring_sparse_table_release(iree_io_uring_sparse_table_t* table,
                                        uint16_t start, uint16_t count);

// Returns the total slot capacity of the table.
static inline uint16_t iree_io_uring_sparse_table_capacity(
    const iree_io_uring_sparse_table_t* table) {
  return (uint16_t)table->bitmap.bit_count;
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_IO_URING_SPARSE_TABLE_H_
