// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_ATOMIC_H_
#define LOOM_IMPORT_CXX_BINDING_ATOMIC_H_

#include <cxx/attributes.h>
#include <cxx/symbols_fwd.h>

#include <optional>
#include <span>
#include <string_view>

#include "loom/import/cxx/value/storage.h"
#include "loom/ops/atomic.h"

namespace loom::cxx_import {

// An admitted scalar integer atomic through a source pointer. The first three
// function template arguments select kind/ordering/scope for RMW and reduction,
// or success/failure/scope for compare-exchange. The concrete signature
// supplies the integer type. Calls retain the pointer's allocation and byte
// origin.
class AtomicIntrinsic {
 public:
  static bool supports(std::string_view name);

  // Unknown operations return nullopt. Invalid signatures and selectors
  // diagnose at owner and throw SourceRejected. The translation unit owns the
  // retained element type and must outlive this binding.
  static std::optional<AtomicIntrinsic> resolve(cxx::TranslationUnit& unit,
                                                Diagnostics& diagnostics,
                                                Types& types,
                                                cxx::FunctionSymbol* function,
                                                const cxx::Attribute& attribute,
                                                cxx::AST* owner);

  // Emits one atomic access using already evaluated source arguments and the
  // ordinary storage projection. Reduction has no result; RMW and CAS return
  // the old memory value. Target lowering owns width, alignment and scope
  // support; no target policy is inferred from the pointer here.
  std::optional<Value> call(std::span<const Value> arguments, Storage& storage,
                            cxx::AST* owner, loom_builder_t* builder,
                            loom_location_id_t location) const;

  bool equivalent(const AtomicIntrinsic& other) const;

 private:
  enum class Operation { Rmw, Reduce, CompareExchange };

  AtomicIntrinsic(Operation operation, const cxx::Type* element_type,
                  loom_type_t type, loom_atomic_kind_t kind,
                  loom_atomic_ordering_t ordering,
                  loom_atomic_ordering_t failure_ordering,
                  loom_atomic_scope_t scope)
      : operation_(operation),
        element_type_(element_type),
        type_(type),
        kind_(kind),
        ordering_(ordering),
        failure_ordering_(failure_ordering),
        scope_(scope) {}

  // Admitted operation family.
  Operation operation_;
  // Source-owned pointee type, including volatile qualification.
  const cxx::Type* element_type_;
  // Signless High representation of the integer payload.
  loom_type_t type_;
  // Integer combining operation; unused for compare-exchange.
  loom_atomic_kind_t kind_;
  // RMW/reduction ordering or successful compare-exchange ordering.
  loom_atomic_ordering_t ordering_;
  // Failed compare-exchange ordering; relaxed for the other operations.
  loom_atomic_ordering_t failure_ordering_;
  // Explicit source synchronization scope, preserved without narrowing.
  loom_atomic_scope_t scope_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_ATOMIC_H_
