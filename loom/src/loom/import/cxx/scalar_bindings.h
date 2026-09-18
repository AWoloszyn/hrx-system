// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SCALAR_BINDINGS_H_
#define LOOM_IMPORT_CXX_SCALAR_BINDINGS_H_

#include "loom/ops/op_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// One generated homogeneous floating-point projection. All operands and the
// result share a supported C++ floating-point type, checked at source
// admission.
typedef struct loom_cxx_scalar_binding_t {
  // Canonical dialect-qualified operation name.
  const char* name;
  // Fixed number of scalar operands.
  uint8_t operand_count;
  // Whether the operation carries canonical scalar fast-math flags.
  bool has_fastmath;
  // Typed operation builder adapted to the fixed operand array.
  iree_status_t (*build)(loom_builder_t* builder, uint8_t flags,
                         const loom_value_id_t* operands,
                         loom_type_t result_type, loom_location_id_t location,
                         loom_op_t** out_op);
} loom_cxx_scalar_binding_t;

// Returns a process-lifetime binding, or NULL for an unprojected operation.
const loom_cxx_scalar_binding_t* loom_cxx_scalar_binding_find(
    iree_string_view_t name);

// Resolves one canonical scalar fast-math flag spelling.
bool loom_cxx_scalar_flag_parse(iree_string_view_t name, uint8_t* out_flag);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_IMPORT_CXX_SCALAR_BINDINGS_H_
