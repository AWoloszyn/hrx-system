// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/atomic.h"

#include <cxx/names.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include <array>
#include <cstdint>
#include <utility>
#include <variant>

#include "loom/import/cxx/source/error.h"
#include "loom/ops/view/ops.h"

namespace loom::cxx_import {

bool AtomicIntrinsic::supports(std::string_view name) {
  return name == "view.atomic.rmw" || name == "view.atomic.reduce" ||
         name == "view.atomic.cmpxchg";
}

std::optional<AtomicIntrinsic> AtomicIntrinsic::resolve(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
    cxx::FunctionSymbol* function, const cxx::Attribute& attribute,
    cxx::AST* owner) {
  auto name = attribute.arguments[0]->name();
  if (!supports(name)) {
    return std::nullopt;
  }
  auto operation = name == "view.atomic.rmw"      ? Operation::Rmw
                   : name == "view.atomic.reduce" ? Operation::Reduce
                                                  : Operation::CompareExchange;
  auto arguments = function->templateArguments();
  if (attribute.arguments.size() != 1 || arguments.size() < 3) {
    diagnostics.reject(unit, owner,
                       "atomic binding requires three leading constant "
                       "template arguments");
  }
  std::array<uint8_t, 3> selectors;
  for (size_t index = 0; index < selectors.size(); ++index) {
    auto value = cxx::template_argument_value(arguments[index]);
    auto* number = value ? std::get_if<std::intmax_t>(&*value) : nullptr;
    if (!number || *number < 0 || *number > UINT8_MAX) {
      diagnostics.reject(unit, owner,
                         "atomic selectors require constant enum values");
    }
    selectors[index] = static_cast<uint8_t>(*number);
  }
  auto kind = LOOM_ATOMIC_KIND_XCHGI;
  auto ordering = LOOM_ATOMIC_ORDERING_RELAXED;
  auto failure_ordering = LOOM_ATOMIC_ORDERING_RELAXED;
  if (operation == Operation::CompareExchange) {
    if (!loom_atomic_ordering_is_valid(selectors[0]) ||
        !loom_atomic_ordering_is_valid(selectors[1])) {
      diagnostics.reject(unit, owner, "unknown atomic ordering");
    }
    ordering = static_cast<loom_atomic_ordering_t>(selectors[0]);
    failure_ordering = static_cast<loom_atomic_ordering_t>(selectors[1]);
    auto error =
        loom_atomic_cmpxchg_ordering_validate(ordering, failure_ordering);
    if (error != LOOM_ATOMIC_CMPXCHG_ORDERING_ERROR_NONE) {
      diagnostics.reject(
          unit, owner,
          string(
              loom_atomic_cmpxchg_ordering_error_expected_constraint(error)));
    }
  } else {
    if (!loom_atomic_kind_is_valid(selectors[0]) ||
        !loom_atomic_kind_accepts_integer(selectors[0])) {
      diagnostics.reject(unit, owner,
                         "atomic binding requires an integer kind");
    }
    if (!loom_atomic_ordering_is_valid(selectors[1])) {
      diagnostics.reject(unit, owner, "unknown atomic ordering");
    }
    kind = static_cast<loom_atomic_kind_t>(selectors[0]);
    ordering = static_cast<loom_atomic_ordering_t>(selectors[1]);
  }
  if (!loom_atomic_scope_is_valid(selectors[2])) {
    diagnostics.reject(unit, owner, "unknown atomic scope");
  }
  auto scope = static_cast<loom_atomic_scope_t>(selectors[2]);

  auto* signature = cxx::type_cast<cxx::FunctionType>(function->type());
  auto parameters = signature->parameterTypes();
  size_t operand_count = operation == Operation::CompareExchange ? 3 : 2;
  if (signature->isVariadic() || parameters.size() != operand_count) {
    diagnostics.reject(unit, owner,
                       "atomic declaration has the wrong operand count");
  }
  auto* pointer =
      cxx::type_cast<cxx::PointerType>(types.unqualified(parameters.back()));
  if (!pointer || unit.typeTraits().is_const(pointer->elementType())) {
    diagnostics.reject(unit, owner,
                       "atomic destination requires a mutable pointer");
  }
  auto* element = types.unqualified(pointer->elementType());
  if (!unit.typeTraits().is_integral(element) ||
      element->kind() == cxx::TypeKind::kBool) {
    diagnostics.reject(unit, owner,
                       "atomic destination requires a non-boolean integer");
  }
  for (size_t index = 0; index + 1 < parameters.size(); ++index) {
    if (types.unqualified(parameters[index]) != element) {
      diagnostics.reject(unit, owner,
                         "atomic operands must match the destination element "
                         "type");
    }
  }
  auto* result = types.unqualified(signature->returnType());
  if (operation == Operation::Reduce ? result->kind() != cxx::TypeKind::kVoid
                                     : result != element) {
    diagnostics.reject(unit, owner,
                       "atomic result must be the destination element type, "
                       "or void for reduction");
  }
  if (((kind == LOOM_ATOMIC_KIND_MINSI || kind == LOOM_ATOMIC_KIND_MAXSI) &&
       types.is_unsigned(element)) ||
      ((kind == LOOM_ATOMIC_KIND_MINUI || kind == LOOM_ATOMIC_KIND_MAXUI) &&
       !types.is_unsigned(element))) {
    diagnostics.reject(unit, owner,
                       "atomic minimum/maximum signedness must match the "
                       "destination element type");
  }
  return AtomicIntrinsic(operation, pointer->elementType(),
                         types.get(element, owner), kind, ordering,
                         failure_ordering, scope);
}

std::optional<Value> AtomicIntrinsic::call(std::span<const Value> arguments,
                                           Storage& storage, cxx::AST* owner,
                                           loom_builder_t* builder,
                                           loom_location_id_t location) const {
  auto access =
      storage.dereference(arguments.back().pointer(), element_type_, owner);
  const int64_t index = 0;
  loom_op_t* op;
  switch (operation_) {
    case Operation::Rmw:
      check(loom_view_atomic_rmw_build(
          builder, 0, kind_, arguments[0].ssa(), access.view, nullptr, 0,
          &index, 1, ordering_, scope_, 0, 0, type_, location, &op));
      return Value(loom_op_results(op)[0]);
    case Operation::Reduce:
      check(loom_view_atomic_reduce_build(
          builder, 0, kind_, arguments[0].ssa(), access.view, nullptr, 0,
          &index, 1, ordering_, scope_, 0, 0, location, &op));
      return std::nullopt;
    case Operation::CompareExchange:
      check(loom_view_atomic_cmpxchg_build(
          builder, 0, arguments[0].ssa(), arguments[1].ssa(), access.view,
          nullptr, 0, &index, 1, ordering_, failure_ordering_, scope_, 0, 0,
          type_, location, &op));
      return Value(loom_op_results(op)[0]);
  }
  std::unreachable();
}

bool AtomicIntrinsic::equivalent(const AtomicIntrinsic& other) const {
  return operation_ == other.operation_ &&
         element_type_ == other.element_type_ && kind_ == other.kind_ &&
         ordering_ == other.ordering_ &&
         failure_ordering_ == other.failure_ordering_ && scope_ == other.scope_;
}

}  // namespace loom::cxx_import
