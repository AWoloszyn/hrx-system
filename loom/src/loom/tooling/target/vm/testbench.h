// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_VM_TESTBENCH_H_
#define LOOM_TOOLING_TARGET_VM_TESTBENCH_H_

#include "iree/vm/buffer.h"
#include "iree/vm/invocation.h"
#include "iree/vm/process.h"
#include "loom/target/provider.h"
#include "loom/tooling/testbench/invocation.h"

#ifdef __cplusplus
extern "C" {
#endif

// Executes ordinary check.case function calls through the VM. One instance
// serves all cases in one parsed module. Its first call compiles an independent
// IR copy specialized to the Core profile with the normal pipeline, verifies
// the emitted bytes, and creates a process; subsequent calls reuse that process
// and its invocation storage. The authored functions need no target binding.
// Buffer arguments and results share storage with the case's HAL bindings.
// Arguments require coherent persistent host mappings; results retain the VM
// storage until the final binding or alias is released.
typedef struct loom_vm_testbench_t {
  // Borrowed compiler capabilities, live through deinitialization.
  const loom_target_environment_t* target_environment;
  // Borrowed case plan identifying exactly which functions cross the host ABI.
  const loom_testbench_module_plan_t* plan;
  // Allocator for bytecode and runtime objects.
  iree_allocator_t host_allocator;
  // Owned process, or NULL until the first function call is prepared.
  iree_vm_process_t* process;
  // Owned reusable execution storage, never shared by concurrent calls.
  iree_vm_invocation_t* invocation;
  // Owned reusable argument variants; also the base of the combined IO slab.
  iree_vm_variant_t* arguments;
  // Result variants within the IO slab, sized from the immutable case plan.
  iree_vm_variant_t* results;
  // Borrowed Core descriptors whose provider lives with the linked VM runtime.
  iree_vm_ref_types_t ref_types;
} loom_vm_testbench_t;

// Initializes a lazy function provider without compiling or allocating.
void loom_vm_testbench_initialize(
    const loom_target_environment_t* target_environment,
    iree_allocator_t host_allocator, loom_vm_testbench_t* out_testbench);

// Releases runtime objects. Safe for a zero-initialized or failed provider.
void loom_vm_testbench_deinitialize(loom_vm_testbench_t* testbench);

// Binds the existing case plan and returns a borrowed function-call callback.
// |user_data| points to an initialized loom_vm_testbench_t. The plan remains
// live through the final call; deinitialization does not access it.
loom_testbench_invocation_provider_t loom_vm_testbench_invocation_provider(
    void* user_data, const loom_testbench_module_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_VM_TESTBENCH_H_
