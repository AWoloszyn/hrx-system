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

#include <array>
#include <type_traits>
#include <utility>
#include <vector>

#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {
namespace {

const cxx::Attribute* operation_attribute(cxx::FunctionSymbol* function) {
  if (!function->attributes()) {
    return nullptr;
  }
  for (const auto& attribute : *function->attributes()) {
    if (attribute.attributeNamespace && attribute.name &&
        attribute.attributeNamespace->name() == "loom" &&
        attribute.name->name() == "op") {
      return &attribute;
    }
  }
  return nullptr;
}

std::span<const loom_value_id_t> flatten(
    std::span<const Value> arguments,
    std::array<loom_value_id_t, 8>& inline_values,
    std::vector<loom_value_id_t>& overflow) {
  size_t count = 0;
  for (auto argument : arguments) {
    count += argument.partition().component_count;
  }
  if (count <= inline_values.size()) {
    size_t index = 0;
    for (auto argument : arguments) {
      for (auto component : argument.components()) {
        inline_values[index++] = component;
      }
    }
    return {inline_values.data(), count};
  }
  overflow.reserve(count);
  for (auto argument : arguments) {
    argument.append_to(overflow);
  }
  return overflow;
}

}  // namespace

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
  const cxx::Attribute* selected = operation_attribute(function);
  if (!selected) {
    return;
  }
  if (!found) {
    diagnostics_.reject(unit_, owner,
                        "operation bindings require leading attributes on a "
                        "plain function declaration");
  }
  if (function->isTemplatePattern()) {
    if (selected->arguments.size() != 1 ||
        (!ViewIntrinsic::supports(selected->arguments[0]->name()) &&
         !AtomicIntrinsic::supports(selected->arguments[0]->name()) &&
         !AssemblyIntrinsic::supports(selected->arguments[0]->name()))) {
      diagnostics_.reject(unit_, owner,
                          "function template operation has no C++ projection");
    }
    auto spelling = std::string(selected->arguments[0]->name());
    auto [entry, inserted] =
        template_bindings_.try_emplace(function->canonical(), spelling);
    if (!inserted && entry->second != spelling) {
      diagnostics_.reject(unit_, owner,
                          "conflicting intrinsic template redeclarations");
    }
    return;
  }
  auto binding = resolve(function, *selected, owner);
  auto [entry, inserted] =
      bindings_.try_emplace(function->canonical(), binding);
  bool equivalent =
      inserted || std::visit(
                      [&](const auto& previous) {
                        using T = std::decay_t<decltype(previous)>;
                        auto* current = std::get_if<T>(&binding);
                        return current && previous.equivalent(*current);
                      },
                      entry->second);
  if (!equivalent) {
    diagnostics_.reject(unit_, owner, "conflicting intrinsic redeclarations");
  }
}

Intrinsics::Binding Intrinsics::resolve(cxx::FunctionSymbol* function,
                                        const cxx::Attribute& attribute,
                                        cxx::AST* owner) {
  auto* signature = cxx::type_cast<cxx::FunctionType>(function->type());
  if (attribute.arguments[0]->name() == "check.expect.equal") {
    auto parameters = signature->parameterTypes();
    if (attribute.arguments.size() != 1 || signature->isVariadic() ||
        signature->returnType()->kind() != cxx::TypeKind::kVoid ||
        parameters.size() != 2 ||
        types_.unqualified(parameters[0]) !=
            types_.unqualified(parameters[1])) {
      diagnostics_.reject(
          unit_, owner,
          "check.expect.equal requires void(T, T) with one scalar type");
    }
    auto type = types_.get(parameters[0], owner);
    if (loom_type_kind(type) != LOOM_TYPE_SCALAR) {
      diagnostics_.reject(unit_, owner,
                          "check.expect.equal requires scalar operands");
    }
    return EqualityBinding{type};
  }
  if (auto* scalar =
          loom_cxx_scalar_binding_find(view(attribute.arguments[0]->name()))) {
    return resolve_scalar(scalar, signature, attribute, owner);
  }
  if (auto shaped = ShapedIntrinsic::resolve(unit_, diagnostics_, types_,
                                             signature, attribute, owner)) {
    return *shaped;
  }
  if (auto assembly = AssemblyIntrinsic::resolve(unit_, diagnostics_, types_,
                                                 function, attribute, owner)) {
    return *assembly;
  }
  if (auto atomic = AtomicIntrinsic::resolve(unit_, diagnostics_, types_,
                                             function, attribute, owner)) {
    return *atomic;
  }
  if (auto view = ViewIntrinsic::resolve(unit_, diagnostics_, types_, signature,
                                         attribute, owner)) {
    return *view;
  }
  diagnostics_.reject(unit_, owner, "operation has no C++ projection");
}

Intrinsics::ScalarBinding Intrinsics::resolve_scalar(
    const loom_cxx_scalar_binding_t* scalar, const cxx::FunctionType* signature,
    const cxx::Attribute& attribute, cxx::AST* owner) {
  ScalarBinding result;
  result.scalar = scalar;
  const auto& traits = unit_.typeTraits();
  const auto* return_type = traits.remove_cv(signature->returnType());
  loom_scalar_type_t scalar_type;
  switch (return_type->kind()) {
    case cxx::TypeKind::kFloat16:
      scalar_type = LOOM_SCALAR_TYPE_F16;
      break;
    case cxx::TypeKind::kFloat:
      scalar_type = LOOM_SCALAR_TYPE_F32;
      break;
    case cxx::TypeKind::kDouble:
      scalar_type = LOOM_SCALAR_TYPE_F64;
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
  result.type = loom_type_scalar(scalar_type);
  return result;
}

std::optional<loom_type_t> Intrinsics::expectation_type(
    cxx::FunctionSymbol* function) const {
  auto entry = bindings_.find(function->canonical());
  if (entry != bindings_.end()) {
    if (auto* binding = std::get_if<EqualityBinding>(&entry->second)) {
      return binding->type;
    }
  }
  return std::nullopt;
}

Intrinsics::Binding* Intrinsics::concrete_binding(cxx::FunctionSymbol* function,
                                                  cxx::AST* owner) {
  auto entry = bindings_.find(function->canonical());
  if (entry != bindings_.end()) {
    return &entry->second;
  }
  if (!function->isSpecialization()) {
    return nullptr;
  }
  auto* primary =
      cxx::symbol_cast<cxx::FunctionSymbol>(function->primaryTemplateSymbol());
  auto pattern = primary ? template_bindings_.find(primary->canonical())
                         : template_bindings_.end();
  if (pattern == template_bindings_.end()) {
    return nullptr;
  }
  auto* attribute = operation_attribute(function);
  if (!attribute || attribute->arguments.size() != 1 ||
      attribute->arguments[0]->name() != pattern->second) {
    diagnostics_.reject(
        unit_, owner,
        "intrinsic specialization does not preserve its template binding");
  }
  auto binding = resolve(function, *attribute, owner);
  auto inserted =
      bindings_.try_emplace(function->canonical(), std::move(binding));
  return &inserted.first->second;
}

Intrinsics::Binding* Intrinsics::lookup(cxx::FunctionSymbol* function,
                                        cxx::AST* owner) {
  return concrete_binding(function, owner);
}

IntrinsicCallResult Intrinsics::call(const Binding& admitted,
                                     std::span<const Value> arguments,
                                     ValueArena& arena, Storage& storage,
                                     cxx::AST* owner, uint8_t math_flags,
                                     loom_builder_t* builder,
                                     loom_location_id_t location) {
  const auto* binding = &admitted;
  if (auto* view = std::get_if<ViewIntrinsic>(binding)) {
    return {view->call(arguments, types_, arena, owner, builder, location)};
  }
  if (auto* atomic = std::get_if<AtomicIntrinsic>(binding)) {
    return {atomic->call(arguments, storage, owner, builder, location)};
  }
  std::array<loom_value_id_t, 8> inline_values;
  std::vector<loom_value_id_t> overflow;
  auto flattened = flatten(arguments, inline_values, overflow);
  if (auto* scalar = std::get_if<ScalarBinding>(binding)) {
    loom_op_t* op;
    check(scalar->scalar->build(builder, scalar->flags | math_flags,
                                flattened.data(), scalar->type, location, &op));
    return {Value(loom_op_results(op)[0])};
  }
  const auto& shaped = std::get<ShapedIntrinsic>(admitted);
  return {Value(shaped.call(flattened, builder, location))};
}

}  // namespace loom::cxx_import
