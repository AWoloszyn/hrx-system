// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared sanitizer race-access vocabulary.

#ifndef LOOM_OPS_SANITIZER_RACE_ACCESS_H_
#define LOOM_OPS_SANITIZER_RACE_ACCESS_H_

#ifdef __cplusplus
extern "C" {
#endif

// Logical memory effect observed by a race detector.
typedef enum loom_sanitizer_race_access_kind_e {
  LOOM_SANITIZER_RACE_ACCESS_KIND_READ = 0,
  LOOM_SANITIZER_RACE_ACCESS_KIND_WRITE = 1,
  LOOM_SANITIZER_RACE_ACCESS_KIND_READ_WRITE = 2,
  LOOM_SANITIZER_RACE_ACCESS_KIND_COUNT_ = 3,
} loom_sanitizer_race_access_kind_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_SANITIZER_RACE_ACCESS_H_
