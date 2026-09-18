// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/replay/atomic_params.h"

iree_status_t iree_hal_replay_atomic_target_error_mode_decode(
    uint16_t version_minor, uint8_t encoded_mode,
    iree_hal_atomic_target_error_mode_t* out_mode) {
  IREE_ASSERT_ARGUMENT(out_mode);
  *out_mode = IREE_HAL_ATOMIC_TARGET_ERROR_MODE_DEFAULT;

  if (IREE_UNLIKELY(
          version_minor <
              IREE_HAL_REPLAY_ATOMIC_TARGET_ERROR_MODE_VERSION_MINOR &&
          encoded_mode != 0)) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "atomic target error mode byte must be zero before replay version 7.3");
  }
  if (IREE_UNLIKELY(encoded_mode >
                    IREE_HAL_ATOMIC_TARGET_ERROR_MODE_INCOMPATIBLE)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid atomic target error mode");
  }
  *out_mode = (iree_hal_atomic_target_error_mode_t)encoded_mode;
  return iree_ok_status();
}
