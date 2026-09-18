// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_PLATFORM_IO_URING_PROACTOR_VALIDATION_H_
#define IREE_ASYNC_PLATFORM_IO_URING_PROACTOR_VALIDATION_H_

#include "iree/async/operation.h"
#include "iree/async/platform/io_uring/proactor.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Validates a caller-provided operation before submission reserves or mutates
// backend state. Successful validation makes SQE encoding infallible; platform
// execution failures remain asynchronous completion results.
iree_status_t iree_async_proactor_io_uring_validate_operation(
    iree_async_proactor_io_uring_t* proactor,
    const iree_async_operation_t* operation);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_IO_URING_PROACTOR_VALIDATION_H_
