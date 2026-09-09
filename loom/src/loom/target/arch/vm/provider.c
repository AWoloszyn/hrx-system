// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/provider.h"

#include "loom/target/arch/vm/descriptors/descriptors.h"
#include "loom/target/arch/vm/lower.h"
#include "loom/target/arch/vm/ops/ops.h"
#include "loom/target/arch/vm/ops/registry.h"

static void loom_vm_descriptor_registry_initialize(
    loom_target_low_descriptor_registry_t* out_registry) {
  static const loom_low_descriptor_set_provider_t kProviders[] = {
      loom_vm_core_descriptor_set,
  };
  loom_target_low_descriptor_registry_initialize_from_tables(
      out_registry, kProviders, IREE_ARRAYSIZE(kProviders));
}

const loom_target_provider_t loom_vm_target_provider = {
    .select_low_call_policy = loom_target_select_low_call_policy_direct,
    .register_context = loom_vm_ops_register_dialect,
    .initialize_low_descriptor_registry =
        loom_vm_descriptor_registry_initialize,
    .initialize_low_lower_policy_registry =
        loom_vm_low_lower_policy_registry_initialize,
    .target_fact_type = &loom_vm_target_fact_type,
};
