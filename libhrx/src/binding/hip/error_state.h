// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef HRX_BINDING_HIP_ERROR_STATE_H_
#define HRX_BINDING_HIP_ERROR_STATE_H_

#include "binding/hip/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Publishes the result of a public HIP call to the calling thread's error
// state and returns the same result.
hipError_t iree_hip_error_state_publish(hipError_t result);

// Completes a public HIP API call through the shared error-state boundary.
#define HIP_RETURN_ERROR(error)                   \
  do {                                            \
    return iree_hip_error_state_publish((error)); \
  } while (0)

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // HRX_BINDING_HIP_ERROR_STATE_H_
