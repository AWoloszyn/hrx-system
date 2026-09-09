// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/records.h"

static const loom_target_snapshot_t kCoreSnapshot = {
    .name = IREE_SVL("vm.core"),
    .codegen_format = LOOM_TARGET_CODEGEN_FORMAT_VM,
    .artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_VM_BINARY,
    .default_pointer_bitwidth = 64,
    .index_bitwidth = 64,
    .offset_bitwidth = 64,
};

static const loom_target_export_plan_t kCoreExportPlan = {
    .name = IREE_SVL("vm-function"),
    .abi_kind = LOOM_TARGET_ABI_VM_FUNCTION,
};

static const loom_target_config_t kCoreConfig = {
    .name = IREE_SVL("vm.core"),
    .contract_set_key = IREE_SVL("vm.core"),
};

static const loom_target_bundle_t kCoreBundle = {
    .name = IREE_SVL("vm-core"),
    .snapshot = &kCoreSnapshot,
    .export_plan = &kCoreExportPlan,
    .config = &kCoreConfig,
};

static const loom_target_bundle_t* const kBundles[] = {NULL, &kCoreBundle};

const loom_target_bundle_table_t loom_vm_target_bundles = {
    .values = kBundles,
    .count = IREE_ARRAYSIZE(kBundles),
};
