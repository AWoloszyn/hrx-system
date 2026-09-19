// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/storage.h"

#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/types.h>

#include "loom/import/cxx/source/error.h"
#include "loom/ir/module.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/index/ops.h"

namespace loom::cxx_import {

StorageAccess Storage::subscript(loom_value_id_t root, loom_value_id_t index,
                                 const cxx::Type* base_type,
                                 const cxx::Type* subscript_type,
                                 cxx::AST* ast) {
  auto* pointer =
      cxx::type_cast<cxx::PointerType>(types_.unqualified(base_type));
  auto* array =
      cxx::type_cast<cxx::BoundedArrayType>(types_.unqualified(base_type));
  if (!pointer && !array) {
    diagnostics_.reject(
        unit_, ast, "subscript base must be a scalar pointer or shared array");
  }
  if (types_.unqualified(subscript_type)->kind() !=
      cxx::TypeKind::kUnsignedInt) {
    diagnostics_.reject(unit_, ast,
                        "pointer subscripts require unsigned int offsets");
  }
  auto* element_type = pointer ? pointer->elementType() : array->elementType();
  auto element = types_.get(element_type, ast);
  auto bytes = unit_.control()->memoryLayout()->sizeOf(element_type);
  if (!bytes) {
    diagnostics_.reject(unit_, ast, "unknown element size");
  }
  auto offset_type = loom_type_scalar(LOOM_SCALAR_TYPE_OFFSET);
  auto index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  loom_op_t* cast;
  // Enter offset first: fixed-width inputs zero-extend there, whereas a
  // direct i32-to-index conversion would interpret unsigned C++ bits as
  // signed.
  check(loom_index_cast_build(&builder_, index,
                              loom_module_value_type(builder_.module, index),
                              offset_type, locations_.get(ast), &cast));
  auto wide_index = loom_op_results(cast)[0];
  check(loom_index_cast_build(&builder_, wide_index, offset_type, index_type,
                              locations_.get(ast), &cast));
  if (array) {
    return {array_views_.at(root), loom_op_results(cast)[0]};
  }
  auto size =
      scalars_.integer(*bytes, LOOM_SCALAR_TYPE_OFFSET, locations_.get(ast));
  loom_op_t* offset;
  check(loom_index_scale_build(&builder_, loom_op_results(cast)[0], size,
                               offset_type, locations_.get(ast), &offset));
  loom_op_t* view;
  auto view_type = loom_type_shaped_1d(LOOM_TYPE_VIEW,
                                       loom_type_element_type(element), 1, 0);
  check(loom_buffer_view_build(&builder_, root, loom_op_results(offset)[0],
                               view_type, locations_.get(ast), &view));
  return {loom_op_results(view)[0], std::nullopt};
}

StorageAllocation Storage::workgroup(const cxx::BoundedArrayType* array,
                                     int64_t explicit_alignment,
                                     cxx::AST* owner) {
  types_.get(array, owner);
  auto* layout = unit_.control()->memoryLayout();
  auto bytes = layout->sizeOf(array);
  auto alignment = layout->alignmentOf(array);
  if (!bytes || !alignment) {
    diagnostics_.reject(unit_, owner, "unknown shared array layout");
  }
  auto length =
      scalars_.integer(*bytes, LOOM_SCALAR_TYPE_OFFSET, locations_.get(owner));
  loom_op_t* op;
  check(loom_buffer_alloca_build(
      &builder_, LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP,
      std::max<int64_t>(*alignment, explicit_alignment), length,
      loom_type_buffer(), locations_.get(owner), &op));
  auto root = loom_op_results(op)[0];
  auto base =
      scalars_.integer(0, LOOM_SCALAR_TYPE_OFFSET, locations_.get(owner));
  auto element = types_.get(array->elementType(), owner);
  auto view_type = loom_type_shaped_1d(
      LOOM_TYPE_VIEW, loom_type_element_type(element), array->size(), 0);
  check(loom_buffer_view_build(&builder_, root, base, view_type,
                               locations_.get(owner), &op));
  auto view = loom_op_results(op)[0];
  array_views_[root] = view;
  return {root, view};
}

}  // namespace loom::cxx_import
