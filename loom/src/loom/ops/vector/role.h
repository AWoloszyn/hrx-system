// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared matrix-fragment role vocabulary.

#ifndef LOOM_OPS_VECTOR_ROLE_H_
#define LOOM_OPS_VECTOR_ROLE_H_

#ifdef __cplusplus
extern "C" {
#endif

// Semantic role of a matrix fragment.
typedef enum loom_vector_role_e {
  LOOM_VECTOR_ROLE_LHS = 0,
  LOOM_VECTOR_ROLE_RHS = 1,
  LOOM_VECTOR_ROLE_INIT = 2,
  LOOM_VECTOR_ROLE_RESULT = 3,
  LOOM_VECTOR_ROLE_COUNT_ = 4,
} loom_vector_role_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_VECTOR_ROLE_H_
