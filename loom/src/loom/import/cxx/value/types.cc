// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/types.h"

#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/types.h>

namespace loom::cxx_import {

const cxx::Type* Types::unqualified(const cxx::Type* type) {
  return unit_.typeTraits().remove_cv(type);
}

loom_type_t Types::get(const cxx::Type* input, cxx::AST* ast) {
  if (!input) {
    diagnostics_.reject(unit_, ast, "expression has no resolved C++ type");
  }
  if (unit_.typeTraits().is_volatile(input)) {
    diagnostics_.reject(unit_, ast, "volatile access is not supported");
  }
  switch (unqualified(input)->kind()) {
    case cxx::TypeKind::kBool:
      return loom_type_scalar(LOOM_SCALAR_TYPE_I1);
    case cxx::TypeKind::kInt:
    case cxx::TypeKind::kUnsignedInt:
      return loom_type_scalar(LOOM_SCALAR_TYPE_I32);
    case cxx::TypeKind::kChar:
    case cxx::TypeKind::kSignedChar:
    case cxx::TypeKind::kUnsignedChar:
      return loom_type_scalar(LOOM_SCALAR_TYPE_I8);
    case cxx::TypeKind::kShortInt:
    case cxx::TypeKind::kUnsignedShortInt:
      return loom_type_scalar(LOOM_SCALAR_TYPE_I16);
    case cxx::TypeKind::kLongInt:
    case cxx::TypeKind::kUnsignedLongInt:
      return loom_type_scalar(unit_.control()->memoryLayout()->sizeOfLong() == 4
                                  ? LOOM_SCALAR_TYPE_I32
                                  : LOOM_SCALAR_TYPE_I64);
    case cxx::TypeKind::kLongLongInt:
    case cxx::TypeKind::kUnsignedLongLongInt:
      return loom_type_scalar(LOOM_SCALAR_TYPE_I64);
    case cxx::TypeKind::kFloat:
      return loom_type_scalar(LOOM_SCALAR_TYPE_F32);
    case cxx::TypeKind::kDouble:
      return loom_type_scalar(LOOM_SCALAR_TYPE_F64);
    case cxx::TypeKind::kFloat16:
      return loom_type_scalar(LOOM_SCALAR_TYPE_F16);
    case cxx::TypeKind::kBoundedArray: {
      auto* array = cxx::type_cast<cxx::BoundedArrayType>(unqualified(input));
      auto element = get(array->elementType(), ast);
      if (loom_type_kind(element) != LOOM_TYPE_SCALAR ||
          loom_type_element_type(element) == LOOM_SCALAR_TYPE_I1) {
        diagnostics_.reject(
            unit_, ast,
            "arrays require a supported non-boolean scalar element");
      }
      return loom_type_buffer();
    }
    case cxx::TypeKind::kPointer: {
      auto* pointer = cxx::type_cast<cxx::PointerType>(unqualified(input));
      auto element = get(pointer->elementType(), ast);
      if (loom_type_kind(element) != LOOM_TYPE_SCALAR ||
          loom_type_element_type(element) == LOOM_SCALAR_TYPE_I1) {
        diagnostics_.reject(
            unit_, ast,
            "pointers require a supported non-boolean scalar element");
      }
      return loom_type_buffer();
    }
    default:
      diagnostics_.reject(unit_, ast,
                          "unsupported C++ type: " + cxx::to_string(input));
  }
}

bool Types::is_unsigned(const cxx::Type* type) {
  return unit_.typeTraits().is_unsigned(type);
}

bool Types::is_float(const cxx::Type* input) {
  auto kind = unqualified(input)->kind();
  return kind == cxx::TypeKind::kFloat || kind == cxx::TypeKind::kFloat16 ||
         kind == cxx::TypeKind::kDouble;
}

}  // namespace loom::cxx_import
