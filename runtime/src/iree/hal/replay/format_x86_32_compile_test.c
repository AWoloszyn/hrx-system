// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/atomic.h"
#include "iree/hal/replay/format.h"

static_assert(sizeof(iree_hal_atomic_wait_params_t) == 24,
              "atomic wait parameter size must remain stable on x86-32");
static_assert(sizeof(iree_hal_atomic_store_params_t) == 16,
              "atomic store parameter size must remain stable on x86-32");
static_assert(sizeof(iree_hal_atomic_rmw_params_t) == 16,
              "atomic RMW parameter size must remain stable on x86-32");
static_assert(offsetof(iree_hal_atomic_wait_params_t, target_error_mode) == 22,
              "atomic wait target error mode offset must remain stable");
static_assert(offsetof(iree_hal_atomic_store_params_t, target_error_mode) == 13,
              "atomic store target error mode offset must remain stable");
static_assert(offsetof(iree_hal_atomic_rmw_params_t, target_error_mode) == 14,
              "atomic RMW target error mode offset must remain stable");
static_assert(_Alignof(iree_hal_atomic_wait_params_t) == _Alignof(uint64_t),
              "atomic wait parameters must retain natural uint64_t alignment");
static_assert(_Alignof(iree_hal_atomic_store_params_t) == _Alignof(uint64_t),
              "atomic store parameters must retain natural uint64_t alignment");
static_assert(_Alignof(iree_hal_atomic_rmw_params_t) == _Alignof(uint64_t),
              "atomic RMW parameters must retain natural uint64_t alignment");

static_assert(sizeof(iree_hal_replay_atomic_wait_params_payload_t) == 24,
              "atomic wait payload size must remain stable on x86-32");
static_assert(sizeof(iree_hal_replay_atomic_store_params_payload_t) == 16,
              "atomic store payload size must remain stable on x86-32");
static_assert(sizeof(iree_hal_replay_atomic_rmw_params_payload_t) == 16,
              "atomic RMW payload size must remain stable on x86-32");
static_assert(offsetof(iree_hal_replay_atomic_wait_params_payload_t,
                       target_error_mode) == 22,
              "atomic wait target error mode offset must remain stable");
static_assert(offsetof(iree_hal_replay_atomic_store_params_payload_t,
                       target_error_mode) == 13,
              "atomic store target error mode offset must remain stable");
static_assert(offsetof(iree_hal_replay_atomic_rmw_params_payload_t,
                       target_error_mode) == 14,
              "atomic RMW target error mode offset must remain stable");
static_assert(_Alignof(iree_hal_replay_atomic_wait_params_payload_t) ==
                  _Alignof(uint64_t),
              "atomic wait payload must retain natural uint64_t alignment");
static_assert(_Alignof(iree_hal_replay_atomic_store_params_payload_t) ==
                  _Alignof(uint64_t),
              "atomic store payload must retain natural uint64_t alignment");
static_assert(_Alignof(iree_hal_replay_atomic_rmw_params_payload_t) ==
                  _Alignof(uint64_t),
              "atomic RMW payload must retain natural uint64_t alignment");
