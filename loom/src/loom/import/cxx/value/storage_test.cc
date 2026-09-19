// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/storage.h"

#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/value/builder_test.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/index/ops.h"

namespace loom::cxx_import {
namespace {
using StorageTest = ValueBuilderTest;

TEST_F(StorageTest, RetainsArrayShapeAndExplicitAlignment) {
  Locations locations(source_.unit(), source_.diagnostics(), module_);
  Scalars scalars(source_.unit(), source_.diagnostics(), types_, locations,
                  builder_);
  Storage storage(source_.unit(), source_.diagnostics(), types_, scalars,
                  locations, builder_);
  auto* control = source_.unit().control();
  auto* owner = source_.unit().ast();
  auto* array_type = control->getBoundedArrayType(control->getFloatType(), 64);
  auto allocation = storage.workgroup(array_type, 64, owner);
  EXPECT_EQ(loom_buffer_alloca_base_alignment(producer(allocation.buffer)), 64);
  auto index = scalars.integer(17, LOOM_SCALAR_TYPE_I32);
  auto access = storage.subscript(allocation.buffer, index, array_type,
                                  control->getUnsignedIntType(), owner);
  EXPECT_EQ(access.view, allocation.view);
  ASSERT_TRUE(access.index.has_value());
  EXPECT_TRUE(loom_index_cast_isa(producer(*access.index)));
  EXPECT_TRUE(loom_type_equal(
      loom_module_value_type(module_, access.view),
      loom_type_shaped_1d(LOOM_TYPE_VIEW, LOOM_SCALAR_TYPE_F32, 64, 0)));
}

TEST_F(StorageTest, PointerProjectionPreservesUnsignedIndexBeforeByteScaling) {
  Locations locations(source_.unit(), source_.diagnostics(), module_);
  Scalars scalars(source_.unit(), source_.diagnostics(), types_, locations,
                  builder_);
  Storage storage(source_.unit(), source_.diagnostics(), types_, scalars,
                  locations, builder_);
  auto* control = source_.unit().control();
  auto* owner = source_.unit().ast();
  auto allocation = storage.workgroup(
      control->getBoundedArrayType(control->getFloatType(), 4), 0, owner);
  auto index = scalars.integer(-1, LOOM_SCALAR_TYPE_I32);
  auto* pointer_type = control->getPointerType(control->getFloatType());
  auto access = storage.subscript(allocation.buffer, index, pointer_type,
                                  control->getUnsignedIntType(), owner);
  EXPECT_FALSE(access.index.has_value());
  auto* view = producer(access.view);
  ASSERT_TRUE(loom_buffer_view_isa(view));
  auto* scale = producer(loom_buffer_view_byte_offset(view));
  ASSERT_TRUE(loom_index_scale_isa(scale));
  auto* signed_index = producer(loom_op_operands(scale)[0]);
  auto* unsigned_offset = producer(loom_op_operands(signed_index)[0]);
  EXPECT_EQ(loom_type_element_type(loom_module_value_type(
                module_, loom_op_results(signed_index)[0])),
            LOOM_SCALAR_TYPE_INDEX);
  EXPECT_EQ(loom_type_element_type(loom_module_value_type(
                module_, loom_op_results(unsigned_offset)[0])),
            LOOM_SCALAR_TYPE_OFFSET);
  EXPECT_EQ(loom_op_operands(unsigned_offset)[0], index);
  EXPECT_THROW(storage.subscript(allocation.buffer, index, pointer_type,
                                 control->getIntType(), owner),
               SourceRejected);
}

}  // namespace
}  // namespace loom::cxx_import
