// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_KERNEL_QUEUE_H_
#define AMDF_SRC_XDNA_UMD_KERNEL_QUEUE_H_

#include <stdint.h>

#include "amdf/amdf.h"
#include "libamdf/src/wait.h"
#include "libamdf/src/xdna/umd/context.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct amdf_xdna_umd_kernel_queue_t amdf_xdna_umd_kernel_queue_t;

// Acquires one native kernel-mediated queue from a scheduling context.
amdf_status_t amdf_xdna_umd_kernel_queue_create(
    amdf_xdna_umd_context_t* context, amdf_xdna_umd_kernel_queue_t** out_queue);

// Frames one validated context-qualified instruction range in the queue's
// preallocated native packet. No instruction bytes are read or modified.
amdf_status_t amdf_xdna_umd_kernel_queue_submit(
    amdf_xdna_umd_kernel_queue_t* queue, uint64_t instruction_address,
    uint32_t instruction_byte_length, uint64_t* out_native_submission);

// Samples the native progress fence with device-to-host acquire semantics.
uint64_t amdf_xdna_umd_kernel_queue_query_progress(
    const amdf_xdna_umd_kernel_queue_t* queue);

// Observes command-owned completion data after native progress proves
// retirement. The caller exclusively owns the pending slot and retains the
// native packet throughout this call. Records execution failure without
// delaying retirement.
void amdf_xdna_umd_kernel_queue_retire_command(
    amdf_xdna_umd_kernel_queue_t* queue);

// Returns the observed terminal device failure without native queries. An
// operation error alone does not fail the queue or establish retirement.
amdf_status_t amdf_xdna_umd_kernel_queue_query_terminal_status(
    const amdf_xdna_umd_kernel_queue_t* queue);

// Waits for one native progress value with caller-selected polling.
amdf_status_t amdf_xdna_umd_kernel_queue_wait(
    amdf_xdna_umd_kernel_queue_t* queue, uint64_t native_submission,
    const amdf_wait_deadline_t* deadline);

// Releases an idle native queue lease.
amdf_status_t amdf_xdna_umd_kernel_queue_destroy(
    amdf_xdna_umd_kernel_queue_t* queue);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_UMD_KERNEL_QUEUE_H_
