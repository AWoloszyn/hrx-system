// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_VIEW_BOUNDARY_TRANSPORT_APPLY_H_
#define LOOM_TRANSFORMS_VIEW_BOUNDARY_TRANSPORT_APPLY_H_

#include "loom/transforms/view/boundary_transport_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Applies one fully preflighted module-wide carrier plan atomically.
iree_status_t loom_view_boundary_apply(loom_view_boundary_plan_t* plan);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_VIEW_BOUNDARY_TRANSPORT_APPLY_H_
