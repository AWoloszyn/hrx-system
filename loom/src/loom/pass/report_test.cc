// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/pass/report.h"

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/text/parser.h"
#include "loom/ir/module.h"
#include "loom/pass/test/harness.h"
#include "loom/pass/tooling.h"

namespace loom {
namespace {

struct ReportAllocator {
  // Zero-based allocation attempt to fail, or the host maximum for no failure.
  iree_host_size_t fail_at = IREE_HOST_SIZE_MAX;
  // Number of allocation attempts.
  iree_host_size_t attempts = 0;
  // Total bytes requested by successful allocation calls.
  iree_host_size_t requested_bytes = 0;
  // Number of allocations that have not been freed.
  iree_host_size_t live = 0;

  iree_allocator_t allocator() { return {this, Control}; }

  static iree_status_t Control(void* self, iree_allocator_command_t command,
                               const void* params, void** pointer) {
    auto* state = static_cast<ReportAllocator*>(self);
    const bool had_allocation = (command == IREE_ALLOCATOR_COMMAND_FREE ||
                                 command == IREE_ALLOCATOR_COMMAND_REALLOC) &&
                                *pointer != nullptr;
    if (command != IREE_ALLOCATOR_COMMAND_FREE &&
        state->attempts++ == state->fail_at) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "report allocation rejected");
    }
    const auto system = iree_allocator_system();
    iree_status_t status = system.ctl(system.self, command, params, pointer);
    if (iree_status_is_ok(status)) {
      if (command == IREE_ALLOCATOR_COMMAND_FREE) {
        state->live -= had_allocation;
      } else {
        state->live += !had_allocation;
        state->requested_bytes +=
            static_cast<const iree_allocator_alloc_params_t*>(params)
                ->byte_length;
      }
    }
    return status;
  }
};

class PassReportTest : public PassTestHarness {};

TEST_F(PassReportTest, FlatPipelineReportOutlivesSourceModules) {
  const char* pipelines[] = {
      "test.module-noop,test.mark-changed",
      "test.module-noop,test.mark-changed,test.fail",
  };
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(pipelines); ++i) {
    SCOPED_TRACE(pipelines[i]);
    PassReportStorage storage;
    loom_module_t* module = nullptr;
    IREE_ASSERT_OK(loom_text_parse(
        IREE_SV("test.func @first() { test.yield }\n"
                "test.func @second() { test.yield }\n"),
        IREE_SV("report.loom"), context(), block_pool(), nullptr, &module));
    ASSERT_NE(module, nullptr);
    loom_test_pass_trace_t trace = {};
    auto options = ToolOptions(&trace);
    options.report = &storage.report;
    loom_pass_run_result_t result = {};
    IREE_EXPECT_STATUS_IS(
        i == 0 ? IREE_STATUS_OK : IREE_STATUS_INTERNAL,
        loom_pass_tool_run_flat_pipeline(
            module, iree_make_cstring_view(pipelines[i]), &options, &result));

    // Release both the source module and cached temporary pipeline storage
    // before reading any names or formatting the retained report.
    loom_module_free(module);
    iree_arena_block_pool_trim(block_pool());
    ASSERT_EQ(storage.report.invocation_count, i == 0 ? 3u : 4u);
    const auto& function_record = storage.report.invocations[1];
    EXPECT_TRUE(iree_string_view_equal(function_record.pipeline_symbol,
                                       IREE_SV("__command_line")));
    EXPECT_TRUE(
        iree_string_view_equal(function_record.symbol_name, IREE_SV("first")));
    ASSERT_EQ(function_record.statistic_count, 2u);
    EXPECT_EQ(function_record.statistics[0].value, 1);
    EXPECT_EQ(function_record.statistics[1].value, 1);
    EXPECT_EQ(function_record.detail_count, 1u);
    EXPECT_EQ(storage.report.invocations[storage.report.invocation_count - 1]
                  .status_code,
              i == 0 ? IREE_STATUS_OK : IREE_STATUS_INTERNAL);

    iree_string_builder_t builder;
    iree_string_builder_initialize(iree_allocator_system(), &builder);
    loom_output_stream_t stream;
    loom_output_stream_for_builder(&builder, &stream);
    IREE_ASSERT_OK(loom_pass_report_format_json(&storage.report, &stream));
    const std::string json(iree_string_builder_buffer(&builder),
                           iree_string_builder_size(&builder));
    EXPECT_NE(json.find("\"pipeline\":\"__command_line\""), std::string::npos);
    EXPECT_NE(json.find("\"symbol\":\"first\""), std::string::npos);
    EXPECT_NE(json.find("\"symbol\":\"second\""), std::string::npos);
    EXPECT_NE(json.find("\"event\":\"synthetic-change\""), std::string::npos);
    iree_string_builder_deinitialize(&builder);
  }
}

TEST_F(PassReportTest, CleansUpEveryReportAllocationFailure) {
  loom_module_t* module =
      Parse(IREE_SV("test.func @first() { test.yield }\n"
                    "test.func @second() { test.yield }\n"));
  ASSERT_NE(module, nullptr);
  for (iree_host_size_t fail_at = 0;; ++fail_at) {
    SCOPED_TRACE(fail_at);
    ReportAllocator allocator;
    allocator.fail_at = fail_at;
    loom_pass_report_t report;
    loom_pass_report_initialize(allocator.allocator(), &report);
    loom_test_pass_trace_t trace = {};
    auto options = ToolOptions(&trace);
    options.report = &report;
    loom_pass_run_result_t result = {};
    iree_status_t status = loom_pass_tool_run_flat_pipeline(
        module, IREE_SV("test.module-noop,test.mark-changed"), &options,
        &result);
    const bool completed = iree_status_is_ok(status);
    if (completed) {
      EXPECT_EQ(report.invocation_count, 3u);
      RecordProperty("report_allocation_count",
                     std::to_string(allocator.attempts));
      RecordProperty("report_requested_bytes",
                     std::to_string(allocator.requested_bytes));
    }
    loom_pass_report_deinitialize(&report);
    EXPECT_EQ(allocator.live, 0u);
    if (completed) {
      break;
    }
    IREE_ASSERT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
  }
}

}  // namespace
}  // namespace loom
