// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/loop_schedule.h"

#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/value/builder_test.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"

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
  auto source_depth = scalars.integer(2, LOOM_SCALAR_TYPE_I32);
  auto source_factor = scalars.integer(3, LOOM_SCALAR_TYPE_I32);
  auto* op = schedule.build(&builder_, lower, upper, step, {}, source_depth,
                            source_factor, LOOM_LOCATION_UNKNOWN);
  auto depth = loom_scf_for_pipeline_depth(op);
  auto factor = loom_scf_for_unroll_factor(op);
  EXPECT_NE(schedule.pipeline_depth(), nullptr);
  EXPECT_NE(schedule.unroll_factor(), nullptr);
  EXPECT_EQ(loom_index_cast_input(producer(depth)), source_depth);
  EXPECT_EQ(loom_index_cast_input(producer(factor)), source_factor);
  EXPECT_EQ(loom_type_element_type(loom_module_value_type(module_, factor)),
            LOOM_SCALAR_TYPE_INDEX);
  EXPECT_EQ(loom_scf_for_unroll_schedule(op),
            LOOM_SCF_FOR_UNROLL_SCHEDULE_RECURRENCE);
}

TEST_F(LoopScheduleTest, UnsignedOperandsWidenBeforeEnteringSignedIndex) {
  Source source(IREE_SV(R"(
    void entry(unsigned factor, unsigned char depth) {
      [[loom::unroll(factor), loom::pipeline(depth)]]
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
  auto depth = scalars.integer(-1, LOOM_SCALAR_TYPE_I8);
  auto factor = scalars.integer(-1, LOOM_SCALAR_TYPE_I32);
  auto* op = schedule.build(&builder_, lower, upper, step, {}, depth, factor,
                            LOOM_LOCATION_UNKNOWN);
  auto widened_depth =
      loom_index_cast_input(producer(loom_scf_for_pipeline_depth(op)));
  auto widened_factor =
      loom_index_cast_input(producer(loom_scf_for_unroll_factor(op)));
  EXPECT_EQ(loom_scalar_extui_input(producer(widened_depth)), depth);
  EXPECT_EQ(loom_scalar_extui_input(producer(widened_factor)), factor);
  EXPECT_EQ(
      loom_type_element_type(loom_module_value_type(module_, widened_depth)),
      LOOM_SCALAR_TYPE_I64);
  EXPECT_EQ(
      loom_type_element_type(loom_module_value_type(module_, widened_factor)),
      LOOM_SCALAR_TYPE_I64);
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

TEST_F(LoopScheduleTest, AnnotationEffectsAreRejectedBeforeTranslation) {
  for (auto text : {
           R"(void entry(volatile unsigned* factor) {
                [[loom::unroll(*factor)]]
                for (unsigned i = 0; i < 16u; ++i) {}
              })",
           R"(struct Factor { operator unsigned(); };
              void entry(Factor factor) {
                [[loom::unroll(static_cast<unsigned>(factor))]]
                for (unsigned i = 0; i < 16u; ++i) {}
              })",
           R"(struct Factor { unsigned operator+(unsigned); };
              void entry(Factor factor) {
                [[loom::unroll(factor + 1u)]]
                for (unsigned i = 0; i < 16u; ++i) {}
              })",
           R"(unsigned operator""_factor(unsigned long long);
              void entry() {
                [[loom::unroll(3_factor)]]
                for (unsigned i = 0; i < 16u; ++i) {}
              })",
       }) {
    SCOPED_TRACE(text);
    Source source(iree_make_cstring_view(text), IREE_SV("schedule.cpp"),
                  options());
    auto* loop = source_loop(source);
    ASSERT_NE(loop, nullptr);
    EXPECT_THROW(
        LoopSchedule(source.unit(), source.diagnostics(), loop->attributeList),
        SourceRejected);
  }
}

}  // namespace
}  // namespace loom::cxx_import
