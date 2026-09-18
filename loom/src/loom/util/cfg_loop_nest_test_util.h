// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_CFG_LOOP_NEST_TEST_UTIL_H_
#define LOOM_UTIL_CFG_LOOP_NEST_TEST_UTIL_H_

#include "loom/util/cfg_loop_nest.h"

namespace loom::testing {

// Independent small-graph oracle. Dominance comes from vertex removal, loop
// members from predecessor closure, and reducibility from topological
// elimination after removing natural backedges. Explicit sets check nesting
// and boundary summaries without contraction, tree intervals or cancellation.
iree_status_t CheckLoopNest(const loom_cfg_loop_nest_t& nest);

}  // namespace loom::testing

#endif  // LOOM_UTIL_CFG_LOOP_NEST_TEST_UTIL_H_
