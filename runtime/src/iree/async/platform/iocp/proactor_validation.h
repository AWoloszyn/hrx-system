// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_PLATFORM_IOCP_PROACTOR_VALIDATION_H_
#define IREE_ASYNC_PLATFORM_IOCP_PROACTOR_VALIDATION_H_

#include "iree/async/platform/iocp/proactor.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Validates an operation before any batch reservation or externally visible
// action. A successfully validated operation can be committed without a
// synchronous API failure; platform failures become terminal completions.
iree_status_t iree_async_proactor_iocp_validate_operation(
    iree_async_proactor_iocp_t* proactor,
    const iree_async_operation_t* operation);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_IOCP_PROACTOR_VALIDATION_H_
