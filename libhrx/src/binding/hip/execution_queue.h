// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef HRX_BINDING_HIP_EXECUTION_QUEUE_H_
#define HRX_BINDING_HIP_EXECUTION_QUEUE_H_

#include "common/stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates a virtual streaming scope backed by an immutable AMDGPU execution
// queue lease. Scopes with equal masks may share one hardware queue.
iree_status_t hrx_hip_execution_queue_scope_create(
    iree_host_size_t device_ordinal,
    iree_host_size_t execution_unit_mask_bit_count,
    const uint32_t* execution_unit_mask, iree_allocator_t host_allocator,
    iree_hal_streaming_queue_scope_t** out_scope);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // HRX_BINDING_HIP_EXECUTION_QUEUE_H_
