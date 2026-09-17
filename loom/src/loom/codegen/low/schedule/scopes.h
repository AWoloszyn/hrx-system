// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_CODEGEN_LOW_SCHEDULE_SCOPES_H_
#define LOOM_CODEGEN_LOW_SCHEDULE_SCOPES_H_

#include "loom/codegen/low/schedule/context.h"

// Projects validated scope phases into issue-only graph edges. Each direct
// member belongs to one phase frontier, and a nested scope joins its parent
// through its end control. Work and edge count are linear in the body size,
// independent of nesting depth. Results remain in the shared schedule table.
iree_status_t loom_low_schedule_build_scope_dependencies(
    loom_low_schedule_build_state_t* state);

#endif  // LOOM_CODEGEN_LOW_SCHEDULE_SCOPES_H_
