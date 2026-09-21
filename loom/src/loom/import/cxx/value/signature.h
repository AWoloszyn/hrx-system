// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_VALUE_SIGNATURE_H_
#define LOOM_IMPORT_CXX_VALUE_SIGNATURE_H_

#include <cxx/types_fwd.h>

#include <span>
#include <vector>

#include "loom/import/cxx/value/types.h"
#include "loom/ops/op_defs.h"

namespace loom::cxx_import {

// A flattened High signature and, when any component type is dependent, the
// destination identities reserved for the immediately following op build.
// The object must remain alive until that build consumes every identity.
struct BoundSignature {
  // High component types in source-value and partition order.
  std::vector<loom_type_t> types;
  // Reserved identities in the same order; empty for a static signature.
  std::vector<loom_value_id_t> identities;
};

// Projects |sources| into one callable or structured-result signature. If a
// source view occurs directly or inside a record, reserves every destination
// identity before binding its dependent shape/layout references. Static-only
// signatures preserve the ordinary builder path and allocate no identities.
BoundSignature bind_signature(Types& types,
                              std::span<const cxx::Type* const> sources,
                              cxx::AST* owner, loom_builder_t* builder);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_VALUE_SIGNATURE_H_
