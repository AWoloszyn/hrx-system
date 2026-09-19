// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/loom-check/comparison.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class ComparisonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loom_check_result_initialize(iree_allocator_system(), &result_);
  }
  void TearDown() override { loom_check_result_deinitialize(&result_); }

  iree_status_t Compare(
      const char* checks, const char* actual,
      loom_test_output_flags_t flags = LOOM_TEST_OUTPUT_CHECKS) {
    loom_test_case_t test_case = {};
    test_case.output_flags = flags;
    test_case.expected = iree_make_cstring_view(checks);
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(&result_.actual_output, actual));
    return loom_check_compare_output(&test_case, iree_allocator_system(),
                                     &result_);
  }

  // Captured comparison verdict and output, owned by the fixture.
  loom_check_result_t result_;
};

TEST_F(ComparisonTest, IndependentTrimmedWholeLineGlobs) {
  IREE_ASSERT_OK(
      Compare("// Only the count fields matter.\n\n"
              "CHECK: row[*] count=5\nCHECK: header ?\nCHECK-NOT: *unknown*\n",
              "header 1\nother data\n  row[3] count=5  \n"));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_PASS);
}

TEST_F(ComparisonTest, CountDoesNotMatchLongerNumber) {
  IREE_ASSERT_OK(Compare("CHECK: * count=5\n", "row count=50\n"));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_FAIL);
  EXPECT_THAT(iree_string_builder_buffer(&result_.detail),
              ::testing::HasSubstr("did not match: * count=5"));
}

TEST_F(ComparisonTest, MatchCannotCrossLines) {
  IREE_ASSERT_OK(Compare("CHECK: begin*end", "begin\nend"));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_FAIL);
}

TEST_F(ComparisonTest, ForbiddenLineFails) {
  IREE_ASSERT_OK(Compare("CHECK: count=5\nCHECK-NOT: unknown=*\n",
                         "count=5\nunknown=1\n"));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_FAIL);
  EXPECT_THAT(
      iree_string_builder_buffer(&result_.detail),
      ::testing::HasSubstr("CHECK-NOT at expected line 2 matched: unknown=1"));
}

TEST_F(ComparisonTest, EmptyOutputFailsPositiveCheck) {
  IREE_ASSERT_OK(Compare("CHECK: *", ""));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_FAIL);
}

TEST_F(ComparisonTest, MissingPositiveCheckIsAnError) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        Compare("// Comment\nCHECK-NOT: absent\n", "present"));
}

TEST_F(ComparisonTest, EmptyPatternIsAnError) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        Compare("CHECK:   \n", "present"));
}

TEST_F(ComparisonTest, UnknownDirectiveIsAnError) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        Compare("CHECK: present\nCHEKC: absent", "present"));
}

TEST_F(ComparisonTest, OrdinaryGoldensDoNotInterpretCheckDirectives) {
  IREE_ASSERT_OK(Compare("CHECK: *", "CHECK: literal", 0));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_FAIL);
}

TEST_F(ComparisonTest, OrdinaryGoldensStillIgnoreStandaloneComments) {
  IREE_ASSERT_OK(Compare("// Comment\nrow\n", "row\n", 0));
  EXPECT_EQ(result_.raw_outcome, LOOM_CHECK_PASS);
}

}  // namespace
