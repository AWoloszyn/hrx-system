// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/types.h"

#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

namespace loom::cxx_import {

const cxx::Type* Types::unqualified(const cxx::Type* type) {
  return unit_.typeTraits().remove_cv(type);
}

const cxx::VectorType* Types::vector(const cxx::Type* type) {
  return cxx::type_cast<cxx::VectorType>(unqualified(type));
}

const Partition& Types::partition(const cxx::Type* input, cxx::AST* owner) {
  if (auto* admitted = record(input, owner)) {
    return *admitted;
  }
  return loom_type_kind(get(input, owner)) == LOOM_TYPE_BUFFER
             ? kPointerPartition
             : kSSAPartition;
}

const RecordPartition* Types::record(const cxx::Type* input, cxx::AST* owner) {
  if (!input) {
    return nullptr;
  }
  auto* type = cxx::type_cast<cxx::ClassType>(unqualified(input));
  if (!type) {
    return nullptr;
  }
  auto traits = unit_.typeTraits();
  if (traits.is_volatile(input)) {
    diagnostics_.reject(unit_, owner, "volatile access is not supported");
  }
  auto* source = type->definition();
  if (auto found = records_.find(source); found != records_.end()) {
    return found->second.get();
  }
  if (!source || !source->isComplete() || source->isUnion() ||
      !source->baseClasses().empty() || !traits.is_aggregate(input) ||
      !traits.is_trivially_copyable(input) ||
      !traits.has_trivial_destructor(input)) {
    diagnostics_.reject(
        unit_, owner,
        "record values require a complete aggregate without unions, bases "
        "or nontrivial lifecycle operations");
  }
  auto result = std::make_unique<RecordPartition>();
  result->kind = ValueKind::Record;
  result->component_count = 0;
  result->source = source;
  for (auto* symbol : source->members()) {
    auto* field = cxx::symbol_cast<cxx::FieldSymbol>(symbol);
    if (!field || field->isStatic()) {
      continue;
    }
    if (field->isBitField() || traits.is_reference(field->type()) ||
        traits.is_array(field->type())) {
      diagnostics_.reject(
          unit_, owner,
          "record value fields cannot be bitfields, references or arrays");
    }
    auto& member_partition = partition(field->type(), owner);
    MemberPartition member{field, &member_partition, result->component_count};
    result->members.push_back(member);
    members_.emplace(field, member);
    append(field->type(), owner, result->component_types);
    auto name = cxx::to_string(field->name());
    if (member_partition.kind == ValueKind::Record) {
      auto& nested = static_cast<const RecordPartition&>(member_partition);
      for (const auto& suffix : nested.component_names) {
        result->component_names.push_back(name + "_" + suffix);
      }
    } else {
      result->component_names.push_back(name);
      if (member_partition.kind == ValueKind::Pointer) {
        result->component_names.push_back(name + "_byte_offset");
      }
    }
    result->component_count += member_partition.component_count;
  }
  auto* admitted = result.get();
  records_.emplace(source, std::move(result));
  return admitted;
}

const MemberPartition& Types::member(cxx::FieldSymbol* field, cxx::AST* owner) {
  if (auto found = members_.find(field); found != members_.end()) {
    return found->second;
  }
  record(field->parent()->type(), owner);
  return members_.at(field);
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
    case cxx::TypeKind::kEnum:
    case cxx::TypeKind::kScopedEnum: {
      auto* underlying = unit_.typeTraits().underlying_type(input);
      if (underlying == unqualified(input)) {
        diagnostics_.reject(unit_, ast,
                            "enum has no resolved underlying representation");
      }
      return get(underlying, ast);
    }
    case cxx::TypeKind::kVector: {
      auto* source = vector(input);
      auto element = get(source->elementType(), ast);
      auto* layout = unit_.control()->memoryLayout();
      auto bytes = layout->sizeOf(source);
      auto element_bytes = layout->sizeOf(source->elementType());
      if (loom_type_kind(element) != LOOM_TYPE_SCALAR ||
          loom_type_element_type(element) == LOOM_SCALAR_TYPE_I1 ||
          !source->elementCount() ||
          source->elementCount() > LOOM_DIM_MAX_STATIC_SIZE || !bytes ||
          !element_bytes || *bytes != source->elementCount() * *element_bytes) {
        diagnostics_.reject(
            unit_, ast,
            "vectors require non-boolean scalar lanes without object padding");
      }
      return loom_type_shaped_1d(LOOM_TYPE_VECTOR,
                                 loom_type_element_type(element),
                                 source->elementCount(), 0);
    }
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
      if ((loom_type_kind(element) != LOOM_TYPE_SCALAR &&
           loom_type_kind(element) != LOOM_TYPE_VECTOR) ||
          loom_type_element_type(element) == LOOM_SCALAR_TYPE_I1) {
        diagnostics_.reject(unit_, ast,
                            "pointers require a supported non-boolean scalar "
                            "or vector element");
      }
      return loom_type_buffer();
    }
    default:
      diagnostics_.reject(unit_, ast,
                          "unsupported C++ type: " + cxx::to_string(input));
  }
}

bool Types::is_unsigned(const cxx::Type* type) {
  auto traits = unit_.typeTraits();
  return traits.is_unsigned(traits.underlying_type(type));
}

void Types::require_mutable(const cxx::Type* input, cxx::AST* owner) {
  if (unit_.typeTraits().is_const(input)) {
    diagnostics_.reject(unit_, owner,
                        "mutation requires a non-const destination");
  }
}

void Types::append(const cxx::Type* input, cxx::AST* owner,
                   std::vector<loom_type_t>& output) {
  if (auto* admitted = record(input, owner)) {
    output.insert(output.end(), admitted->component_types.begin(),
                  admitted->component_types.end());
    return;
  }
  auto type = get(input, owner);
  output.push_back(type);
  if (loom_type_kind(type) == LOOM_TYPE_BUFFER) {
    output.push_back(loom_type_scalar(LOOM_SCALAR_TYPE_OFFSET));
  }
}

bool Types::is_float(const cxx::Type* input) {
  auto kind = unqualified(input)->kind();
  return kind == cxx::TypeKind::kFloat || kind == cxx::TypeKind::kFloat16 ||
         kind == cxx::TypeKind::kDouble;
}

}  // namespace loom::cxx_import
