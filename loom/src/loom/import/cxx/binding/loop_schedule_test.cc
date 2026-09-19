// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/loop_schedule.h"

#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/value/builder_test.h"
#include "loom/ops/index/ops.h"

namespace loom::cxx_import {
namespace {
using LoopScheduleTest = ValueBuilderTest;

cxx::ForStatementAST* source_loop(Source& source) {
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    if (auto* function =
            cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration)) {
      auto* body = cxx::ast_cast<cxx::CompoundStatementFunctionBodyAST>(
                       function->functionBody)
                       ->statement;
      return cxx::ast_cast<cxx::ForStatementAST>(body->statementList->value);
    }
  }
  return nullptr;
}

TEST_F(LoopScheduleTest, PoliciesBecomeTypedSSAOperandsAndExplicitOrdering) {
  Source source(IREE_SV(R"(
    void entry() {
      [[loom::unroll(3), loom::pipeline(2), loom::schedule("recurrence")]]
      for (unsigned i = 0; i < 16u; ++i) {}
    }
  )"),
                IREE_SV("schedule.cpp"), options());
  auto* loop = source_loop(source);
  ASSERT_NE(loop, nullptr);
  LoopSchedule schedule(source.unit(), source.diagnostics(),
                        loop->attributeList);
  Locations locations(source_.unit(), source_.diagnostics(), module_);
  Scalars scalars(source_.unit(), source_.diagnostics(), types_, locations,
                  builder_);
  auto lower = scalars.integer(0, LOOM_SCALAR_TYPE_OFFSET);
  auto upper = scalars.integer(16, LOOM_SCALAR_TYPE_OFFSET);
  auto step = scalars.integer(1, LOOM_SCALAR_TYPE_OFFSET);
  auto* op =
      schedule.build(&builder_, lower, upper, step, {}, LOOM_LOCATION_UNKNOWN);
  auto depth = loom_scf_for_pipeline_depth(op);
  auto factor = loom_scf_for_unroll_factor(op);
  EXPECT_EQ(loom_index_constant_value(producer(depth)).i64, 2);
  EXPECT_EQ(loom_index_constant_value(producer(factor)).i64, 3);
  EXPECT_EQ(loom_type_element_type(loom_module_value_type(module_, factor)),
            LOOM_SCALAR_TYPE_INDEX);
  EXPECT_EQ(loom_scf_for_unroll_schedule(op),
            LOOM_SCF_FOR_UNROLL_SCHEDULE_RECURRENCE);
}

TEST_F(LoopScheduleTest, OrderingRequiresAnExplicitUnrollChoice) {
  Source source(
      IREE_SV(
          R"(void entry() { [[loom::schedule("linear")]] for (unsigned i=0; i<8u; ++i) {} })"),
      IREE_SV("schedule.cpp"), options());
  auto* loop = source_loop(source);
  ASSERT_NE(loop, nullptr);
  EXPECT_THROW(
      LoopSchedule(source.unit(), source.diagnostics(), loop->attributeList),
      SourceRejected);
}

}  // namespace
}  // namespace loom::cxx_import
