// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/lower.h"

#include "loom/error/error_catalog.h"
#include "loom/target/arch/vm/contracts/core.h"
#include "loom/target/arch/vm/contracts/core_lower_rules.h"
#include "loom/target/arch/vm/descriptors/descriptors.h"

static iree_status_t loom_vm_map_type(void* user_data,
                                      loom_low_lower_context_t* context,
                                      const loom_op_t* source_op,
                                      loom_type_t source_type,
                                      loom_type_t* out_low_type) {
  (void)user_data;
  // Both address domains use the target's 64-bit value carrier. Signedness is
  // expressed by the selected operations, not a distinct physical register.
  if (loom_type_is_scalar(source_type) &&
      (loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_INDEX ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_OFFSET)) {
    source_type = loom_type_scalar(LOOM_SCALAR_TYPE_I64);
  }
  if (loom_type_is_scalar(source_type) &&
      (loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_I1 ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_I8 ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_I16 ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_I32 ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_I64 ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_F32 ||
       loom_type_element_type(source_type) == LOOM_SCALAR_TYPE_F64)) {
    return loom_low_lower_make_typed_register_type(
        context, VM_CORE_REG_CLASS_ID_VALUE, 1, source_type, out_low_type);
  }
  return loom_low_lower_emit_source_type_unsupported(
      context, source_op, IREE_SV("source"), source_type);
}

static const loom_low_lower_rule_set_t* const kRuleSets[] = {
    &loom_vm_core_lower_rule_set,
};

static const loom_target_contract_binding_t kContractBindings[] = {
    {.fragment = &loom_vm_core_contract_fragment, .rule_set_index = 0},
};

static const loom_low_lower_policy_t kPolicy = {
    .name = IREE_SVL("vm-lower"),
    .error_catalog = &loom_error_catalog_core,
    .map_type = {.fn = loom_vm_map_type},
    .rule_sets = {.count = IREE_ARRAYSIZE(kRuleSets), .values = kRuleSets},
    .contract_bindings = kContractBindings,
    .contract_binding_count = IREE_ARRAYSIZE(kContractBindings),
};

void loom_vm_low_lower_policy_registry_initialize(
    loom_low_lower_policy_registry_t* out_registry) {
  static const loom_low_lower_policy_registry_entry_t kEntries[] = {
      {.contract_set_key = IREE_SVL("vm.core"), .policy = &kPolicy},
  };
  loom_low_lower_policy_registry_initialize_from_entries(
      out_registry, kEntries, IREE_ARRAYSIZE(kEntries));
}
