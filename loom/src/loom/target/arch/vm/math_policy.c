// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/math_policy.h"

static void loom_vm_math_policy_query(
    const loom_target_math_policy_t* policy,
    const loom_target_math_query_t* query,
    loom_target_math_policy_decision_t* out_decision) {
  switch (query->math_op) {
    case LOOM_TARGET_MATH_OP_ADDF:
    case LOOM_TARGET_MATH_OP_MULF:
    case LOOM_TARGET_MATH_OP_CEILF:
    case LOOM_TARGET_MATH_OP_FLOORF:
    case LOOM_TARGET_MATH_OP_ROUNDEVENF:
    case LOOM_TARGET_MATH_OP_TRUNCF:
      break;
    default:
      *out_decision = (loom_target_math_policy_decision_t){
          .action = LOOM_TARGET_MATH_POLICY_ACTION_REJECT,
          .constraint_key = IREE_SVL("math.op.supported"),
      };
      return;
  }
  if (query->element_type != LOOM_SCALAR_TYPE_F32 &&
      query->element_type != LOOM_SCALAR_TYPE_F64) {
    *out_decision = (loom_target_math_policy_decision_t){
        .action = LOOM_TARGET_MATH_POLICY_ACTION_REJECT,
        .constraint_key = IREE_SVL("math.element.f32_f64"),
    };
    return;
  }
  *out_decision = (loom_target_math_policy_decision_t){
      .action = LOOM_TARGET_MATH_POLICY_ACTION_KEEP,
      .constraint_key = IREE_SVL("math.op.selected_width"),
  };
}

static const loom_target_math_policy_t loom_vm_math_policy = {
    .name = IREE_SVL("vm-math"),
    .query = loom_vm_math_policy_query,
};

void loom_vm_math_policy_registry_initialize(
    loom_target_math_policy_registry_t* out_registry) {
  static const loom_target_math_policy_registry_entry_t kEntries[] = {
      {.contract_set_key = IREE_SVL("vm.core"), .policy = &loom_vm_math_policy},
  };
  loom_target_math_policy_registry_initialize_from_entries(
      out_registry, kEntries, IREE_ARRAYSIZE(kEntries));
}
