// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_VALUE_TYPES_H_
#define LOOM_IMPORT_CXX_VALUE_TYPES_H_

#include <cxx/symbols_fwd.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "loom/import/cxx/source/source.h"
#include "loom/import/cxx/value/representation.h"
#include "loom/ir/types.h"

namespace loom::cxx_import {

// One source member's retained position in an admitted record value. Component
// slices describe High value transport, independently of C++ object offsets.
struct MemberPartition {
  // Declaration identity and cv-qualified source type, borrowed from Source.
  cxx::FieldSymbol* field;
  // Admitted member structure, owned by Types or a static leaf partition.
  const Partition* partition;
  // First component of this member in its containing record binding.
  size_t component_offset;
};

// Canonical source record structure, built once at admission. This static
// record subset contains no dependent High types. Future shaped source values
// must bind dependent types from their own Value identities, not cache another
// binding's extents or layouts in this source schema.
struct RecordPartition final : Partition {
  // Canonical definition retaining nominal identity and source object layout.
  cxx::ClassSymbol* source;
  // Declaration-order members, including zero-component empty records.
  std::vector<MemberPartition> members;
  // Flattened static High signature; no physical ABI or host object padding.
  std::vector<loom_type_t> component_types;
  // Retained member paths used to name newly bound High components.
  std::vector<std::string> component_names;
};

// Projects resolved C++ types using the translation unit's explicit data model.
// Source signedness remains available even when both types share one IR
// carrier. Enums use their resolved underlying integer representation; nominal
// source types remain intact for C++ semantic queries. The source unit and
// diagnostics outlive this projection.
class Types {
 public:
  Types(cxx::TranslationUnit& unit, Diagnostics& diagnostics)
      : unit_(unit), diagnostics_(diagnostics) {}

  // Diagnoses unsupported source representations at owner and throws
  // SourceRejected. Returned types retain no source storage.
  loom_type_t get(const cxx::Type* input, cxx::AST* owner);
  // Admits a source value and returns its stable, identity-free partition.
  // Leaf carriers are static; admitted records are owned by this Types object.
  const Partition& partition(const cxx::Type* input, cxx::AST* owner);
  // Returns a record's admitted source schema, or null for a non-record type.
  const RecordPartition* record(const cxx::Type* input, cxx::AST* owner);
  // Direct lookup of a member slice retained by its owning record's admission.
  const MemberPartition& member(cxx::FieldSymbol* field, cxx::AST* owner);
  // Admits mutation of the source object before projection removes qualifiers.
  // A const pointer binding is immutable; a pointer to const has an immutable
  // pointee but the binding itself may still change.
  void require_mutable(const cxx::Type* input, cxx::AST* owner);
  // Appends today's admitted static High signature for an ordinary function
  // or structured result. A dependent source value requires destination SSA
  // identities before type binding and cannot use this static projection.
  // Kernel parameters instead use get(), retaining the launch binding ABI.
  void append(const cxx::Type* input, cxx::AST* owner,
              std::vector<loom_type_t>& output);
  const cxx::Type* unqualified(const cxx::Type* type);
  // Returns the resolved vector representation, or null for a scalar/object.
  const cxx::VectorType* vector(const cxx::Type* type);
  bool is_unsigned(const cxx::Type* type);
  bool is_float(const cxx::Type* type);

 private:
  // Resolved source traits and configured memory layout.
  cxx::TranslationUnit& unit_;
  // Source rejection boundary for unsupported types.
  Diagnostics& diagnostics_;
  // Stable source partitions, independent of every particular SSA binding.
  std::unordered_map<cxx::ClassSymbol*, std::unique_ptr<RecordPartition>>
      records_;
  // Member identity indexes the slice established by record admission.
  std::unordered_map<cxx::FieldSymbol*, MemberPartition> members_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_VALUE_TYPES_H_
