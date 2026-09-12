// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_LOWER_H_
#define LOOM_TARGET_ARCH_VM_LOWER_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

// Binds vm.core to spec-derived lowering rules and VM register type mapping.
// The shared lowerer owns traversal, value mapping, and function/control-flow
// structure. Semantic scalar types survive in the value register type so the
// module writer can describe the public function ABI without rediscovery.
// Analyzed views alias their buffer carrier; the shared source-memory plan
// supplies each access's complete byte coordinate without a runtime view
// object.
// Read-only data loads retain symbol references until module emission assigns
// ordinals; value globals are not reinterpreted as byte payloads.
void loom_vm_low_lower_policy_registry_initialize(
    loom_low_lower_policy_registry_t* out_registry);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_LOWER_H_
