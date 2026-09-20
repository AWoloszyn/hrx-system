// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/input/input.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(InputOptionsTest, RejectsUnlinkedSuffixesAndSupportsExplicitText) {
  const loom_input_provider_t* provider = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNIMPLEMENTED,
      loom_input_provider_select({}, {}, IREE_SV("source.cc"), &provider));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNIMPLEMENTED,
                        loom_input_provider_select(
                            {}, {}, IREE_SV("source.cxx-test"), &provider));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNIMPLEMENTED,
      loom_input_provider_select({}, IREE_SV("cxx"), {}, &provider));
  IREE_ASSERT_OK(loom_input_provider_select({}, IREE_SV("loom"),
                                            IREE_SV("source.txt"), &provider));
  EXPECT_EQ(provider, &loom_input_text_provider);
  IREE_ASSERT_OK(
      loom_input_provider_select({}, {}, IREE_SV("<stdin>"), &provider));
  EXPECT_EQ(provider, &loom_input_text_provider);
}

TEST(InputOptionsTest, KeepsLanguageOptionsIndependent) {
  const iree_string_view_t entries[] = {IREE_SV("cxx:std=c++20 D=VALUE=7"),
                                        IREE_SV("other:mode=fast")};
  iree_string_view_t options;
  IREE_ASSERT_OK(loom_input_options_for_provider(
      {IREE_ARRAYSIZE(entries), entries}, IREE_SV("cxx"), &options));
  EXPECT_TRUE(iree_string_view_equal(options, IREE_SV("std=c++20 D=VALUE=7")));
  IREE_ASSERT_OK(loom_input_options_for_provider(
      {IREE_ARRAYSIZE(entries), entries}, IREE_SV("loom"), &options));
  EXPECT_TRUE(iree_string_view_is_empty(options));
  const iree_string_view_t duplicates[] = {IREE_SV("cxx:"),
                                           IREE_SV("cxx:std=c++20")};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_input_options_for_provider({IREE_ARRAYSIZE(duplicates), duplicates},
                                      IREE_SV("cxx"), &options));
  const iree_string_view_t malformed[] = {IREE_SV("std=c++20")};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_input_options_for_provider({IREE_ARRAYSIZE(malformed), malformed},
                                      IREE_SV("cxx"), &options));
}

}  // namespace
