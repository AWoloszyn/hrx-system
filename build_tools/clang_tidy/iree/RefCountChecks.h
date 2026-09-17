// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_BUILD_TOOLS_CLANG_TIDY_IREE_REF_COUNT_CHECKS_H_
#define IREE_BUILD_TOOLS_CLANG_TIDY_IREE_REF_COUNT_CHECKS_H_

#include "clang-tidy/ClangTidyCheck.h"

namespace clang::tidy::iree {

// Identifies release operations by the ownership naming contract. Release-wait
// helpers observe completion without consuming the caller's reference.
bool IsRefCountReleaseFunctionName(StringRef FunctionName);

class RefCountLifecycleCheck final : public ClangTidyCheck {
 public:
  RefCountLifecycleCheck(StringRef Name, ClangTidyContext* Context);

  void registerMatchers(ast_matchers::MatchFinder* Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace clang::tidy::iree

#endif  // IREE_BUILD_TOOLS_CLANG_TIDY_IREE_REF_COUNT_CHECKS_H_
