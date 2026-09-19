// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/launch.h"

#include "loom/import/cxx/binding/declaration_test.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/kernel/ops.h"

namespace loom::cxx_import {
namespace {
using LaunchTest = ValueBuilderTest;

TEST_F(LaunchTest, RedeclarationsSupplyOneKernelConfiguration) {
  Source source(IREE_SV(R"(
    [[loom::workgroup_size(64, 1, 1)]] void entry();
    [[loom::workgroup_count(2, 1, 1)]] void entry();
  )"),
                IREE_SV("launch.cpp"), options());
  auto declared = declarations(source, "entry");
  ASSERT_EQ(declared.size(), 2u);
  LaunchContracts launches(source.unit(), source.diagnostics());
  for (const auto& declaration : declared) {
    launches.declaration(declaration.function, declaration.attributes);
  }
  loom_string_id_t name;
  IREE_ASSERT_OK(loom_module_intern_string(module_, IREE_SV("entry"), &name));
  loom_symbol_id_t symbol;
  IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
  loom_op_t* kernel;
  IREE_ASSERT_OK(loom_kernel_def_build(&builder_, 0, 0, {}, 0, 0, {0, symbol},
                                       nullptr, 0, nullptr, 0, nullptr, 0,
                                       LOOM_LOCATION_UNKNOWN, &kernel));
  auto* config = loom_kernel_def_config(kernel);
  auto saved = loom_builder_enter_region(&builder_, kernel, config);
  launches.build(declared.back().function, "entry", &builder_,
                 LOOM_LOCATION_UNKNOWN);
  loom_builder_restore(&builder_, saved);
  auto* launch = loom_region_entry_block(config)->last_op;
  ASSERT_TRUE(loom_kernel_launch_config_isa(launch));
  EXPECT_EQ(loom_index_constant_value(
                producer(loom_kernel_launch_config_workgroup_count_x(launch)))
                .i64,
            2);
  EXPECT_EQ(loom_index_constant_value(
                producer(loom_kernel_launch_config_workgroup_size_x(launch)))
                .i64,
            64);
}

}  // namespace
}  // namespace loom::cxx_import
