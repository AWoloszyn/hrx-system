// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_REPLAY_ATOMIC_PARAMS_H_
#define IREE_HAL_REPLAY_ATOMIC_PARAMS_H_

#include <stdint.h>

#include "iree/base/api.h"
#include "iree/hal/atomic.h"
#include "iree/hal/replay/format.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Decodes a serialized atomic target-error mode for |version_minor|.
//
// Before version 7.3 the payload byte was reserved and must be zero. Starting
// with version 7.3 the byte stores an iree_hal_atomic_target_error_mode_t.
iree_status_t iree_hal_replay_atomic_target_error_mode_decode(
    uint16_t version_minor, uint8_t encoded_mode,
    iree_hal_atomic_target_error_mode_t* out_mode);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_REPLAY_ATOMIC_PARAMS_H_
