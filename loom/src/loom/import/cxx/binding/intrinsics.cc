// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/intrinsics.h"

#include <cxx/ast.h>
#include <cxx/attributes.h>
#include <cxx/names.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {

void Intrinsics::declaration(cxx::FunctionSymbol* function,
                             cxx::List<cxx::AttributeSpecifierAST*>* attributes,
                             cxx::AST* owner) {
  bool found = false;
  visit_loom_attributes(
      unit_, attributes,
      [&](std::string_view name, cxx::AttributeAST* attribute) {
        if (name != "op") {
          return;
        }
        if (found || function->declaration()) {
          diagnostics_.reject(unit_, owner,
                              "an intrinsic requires one operation binding and "
                              "no function body");
        }
        found = true;
        auto* clause = attribute->attributeArgumentClause;
        if (!clause || !clause->expressionList) {
          diagnostics_.reject(unit_, owner,
                              "loom::op requires a string operation name");
        }
        for (auto* argument : cxx::ListView{clause->expressionList}) {
          if (!cxx::ast_cast<cxx::StringLiteralExpressionAST>(argument)) {
            diagnostics_.reject(
                unit_, argument,
                "operation bindings accept only string arguments");
          }
        }
      });
  const cxx::Attribute* selected = nullptr;
  if (function->attributes()) {
    for (const auto& attribute : *function->attributes()) {
      if (attribute.attributeNamespace && attribute.name &&
          attribute.attributeNamespace->name() == "loom" &&
          attribute.name->name() == "op") {
        selected = &attribute;
        break;
      }
    }
  }
  if (!selected) {
    return;
  }
  if (!found) {
    diagnostics_.reject(unit_, owner,
                        "operation bindings require leading attributes on a "
                        "plain function declaration");
  }
  auto binding = resolve(function, *selected, owner);
  auto [entry, inserted] =
      bindings_.try_emplace(function->canonical(), binding);
  if (!inserted && (entry->second.scalar != binding.scalar ||
                    entry->second.flags != binding.flags ||
                    !loom_type_equal(entry->second.type, binding.type))) {
    diagnostics_.reject(unit_, owner, "conflicting intrinsic redeclarations");
  }
}

Intrinsics::Binding Intrinsics::resolve(cxx::FunctionSymbol* function,
                                        const cxx::Attribute& attribute,
                                        cxx::AST* owner) {
  Binding result;
  result.scalar =
      loom_cxx_scalar_binding_find(view(attribute.arguments[0]->name()));
  if (!result.scalar) {
    diagnostics_.reject(unit_, owner, "operation has no scalar C++ projection");
  }
  auto* signature = cxx::type_cast<cxx::FunctionType>(function->type());
  const auto& traits = unit_.typeTraits();
  const auto* return_type = traits.remove_cv(signature->returnType());
  loom_scalar_type_t scalar;
  switch (return_type->kind()) {
    case cxx::TypeKind::kFloat16:
      scalar = LOOM_SCALAR_TYPE_F16;
      break;
    case cxx::TypeKind::kFloat:
      scalar = LOOM_SCALAR_TYPE_F32;
      break;
    case cxx::TypeKind::kDouble:
      scalar = LOOM_SCALAR_TYPE_F64;
      break;
    default:
      diagnostics_.reject(
          unit_, owner,
          "scalar intrinsic result must be _Float16, float, or double");
  }
  if (signature->isVariadic() ||
      signature->parameterTypes().size() != result.scalar->operand_count) {
    diagnostics_.reject(unit_, owner,
                        "intrinsic declaration has the wrong operand count");
  }
  for (const auto* parameter : signature->parameterTypes()) {
    if (traits.remove_cv(parameter) != return_type) {
      diagnostics_.reject(
          unit_, owner,
          "intrinsic operands must have the result's floating-point type");
    }
  }
  for (size_t index = 1; index < attribute.arguments.size(); ++index) {
    uint8_t flag;
    if (!result.scalar->has_fastmath ||
        !loom_cxx_scalar_flag_parse(view(attribute.arguments[index]->name()),
                                    &flag)) {
      diagnostics_.reject(unit_, owner,
                          "intrinsic has an unsupported fast-math flag");
    }
    result.flags |= flag;
  }
  result.type = loom_type_scalar(scalar);
  return result;
}

std::optional<loom_value_id_t> Intrinsics::call(
    cxx::FunctionSymbol* function, std::span<const loom_value_id_t> arguments,
    uint8_t math_flags, loom_builder_t* builder, loom_location_id_t location) {
  auto entry = bindings_.find(function->canonical());
  if (entry == bindings_.end()) {
    return std::nullopt;
  }
  const auto& binding = entry->second;
  loom_op_t* op;
  check(binding.scalar->build(builder, binding.flags | math_flags,
                              arguments.data(), binding.type, location, &op));
  return loom_op_results(op)[0];
}

}  // namespace loom::cxx_import
