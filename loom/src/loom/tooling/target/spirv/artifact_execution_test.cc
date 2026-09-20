// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdint>
#include <string>
#include <vector>

#include "iree/hal/cts/util/test_base.h"
#include "loom/tooling/target/spirv/multiple_entries_spv.h"

namespace iree::hal::cts {
namespace {

// Uses a complete loom-compile kernel artifact, so every dispatch below shares
// the same shader module and executable. Per-invocation JIT checks cannot
// exercise this boundary.
class SpirvArtifactExecutionTest : public CtsTestBase<> {
 protected:
  void SetUp() override {
    CtsTestBase::SetUp();
    if (HasFatalFailure() || IsSkipped()) {
      return;
    }
    dispatch_queue_ =
        QueueForCommandCategories(IREE_HAL_COMMAND_CATEGORY_DISPATCH);
    ASSERT_NE(dispatch_queue_, nullptr);
    const auto target =
        SelectExecutableTarget(IREE_SV("spirv"), IREE_SV("vulkan1.3+bda"));
    ASSERT_EQ(target.outcome,
              IREE_HAL_EXECUTABLE_TARGET_SELECTION_OUTCOME_SELECTED);
    const iree_file_toc_t* artifact = loom_spirv_multiple_entries_create();
    ASSERT_EQ(loom_spirv_multiple_entries_size(), 1u);
    IREE_ASSERT_OK(LoadExecutable(
        iree_hal_queue_family(dispatch_queue_), target.target,
        IREE_HAL_EXECUTABLE_LOAD_FLAG_NONE,
        iree_make_const_byte_span(artifact[0].data, artifact[0].size),
        executable_.out()));
  }

  void TearDown() override {
    executable_.reset();
    CtsTestBase::TearDown();
  }

  void Dispatch(iree_string_view_t name, iree_const_byte_span_t constants,
                iree_hal_buffer_ref_list_t bindings) {
    iree_hal_executable_function_t function;
    IREE_ASSERT_OK(iree_hal_executable_lookup_function_by_name(
        executable_, name, &function));
    SemaphoreList completion(device_, {0}, {1});
    IREE_ASSERT_OK(iree_hal_queue_dispatch(
        dispatch_queue_, iree_hal_semaphore_list_empty(), completion,
        executable_, function, iree_hal_make_static_dispatch_config(1, 1, 1),
        constants, bindings, IREE_HAL_DISPATCH_FLAG_NONE));
    IREE_ASSERT_OK(iree_hal_semaphore_list_wait(
        completion, iree_infinite_timeout(), IREE_ASYNC_WAIT_FLAG_NONE));
  }

  // Dispatch queue borrowed from the fixture's device.
  iree_hal_queue_t* dispatch_queue_ = nullptr;
  // One loaded module containing all three exported kernels.
  Ref<iree_hal_executable_t> executable_;
};

TEST_P(SpirvArtifactExecutionTest, DispatchesDistinctEntrySignatures) {
  ASSERT_EQ(iree_hal_executable_function_count(executable_), 3u);
  iree_hal_executable_function_t helper;
  IREE_EXPECT_STATUS_IS(StatusCode::kNotFound,
                        iree_hal_executable_lookup_function_by_name(
                            executable_, IREE_SV("twice"), &helper));
  IREE_EXPECT_STATUS_IS(
      StatusCode::kNotFound,
      iree_hal_executable_lookup_function_by_name(
          executable_, IREE_SV("different_signatures"), &helper));
  IREE_EXPECT_STATUS_IS(StatusCode::kNotFound,
                        iree_hal_executable_lookup_function_by_name(
                            executable_, IREE_SV("combine"), &helper));

  Ref<iree_hal_buffer_t> first;
  Ref<iree_hal_buffer_t> second;
  Ref<iree_hal_buffer_t> third;
  IREE_ASSERT_OK(CreateZeroedDeviceBuffer(sizeof(int32_t), first.out()));
  IREE_ASSERT_OK(CreateZeroedDeviceBuffer(sizeof(int32_t), second.out()));
  IREE_ASSERT_OK(CreateZeroedDeviceBuffer(sizeof(int32_t), third.out()));
  iree_hal_buffer_ref_t bindings[] = {
      iree_hal_make_buffer_ref(first, 0, sizeof(int32_t)),
      iree_hal_make_buffer_ref(second, 0, sizeof(int32_t)),
      iree_hal_make_buffer_ref(third, 0, sizeof(int32_t)),
  };
  const int32_t constants[] = {4, 5};
  Dispatch(IREE_SV("fill"), iree_const_byte_span_empty(), {1, bindings});
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(ReadBufferData<int32_t>(first), std::vector<int32_t>({6}));
  Dispatch(IREE_SV("add"),
           iree_make_const_byte_span(constants, sizeof(constants[0])),
           {2, bindings});
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(ReadBufferData<int32_t>(second), std::vector<int32_t>({10}));
  Dispatch(IREE_SV("@combined.alias[0]"),
           iree_make_const_byte_span(constants, sizeof(constants)),
           {3, bindings});
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(ReadBufferData<int32_t>(third), std::vector<int32_t>({25}));

  // Check both under-sized and over-sized calls. The largest entry's ABI must
  // never become the dispatch contract for the other entries in its module.
  const iree_string_view_t names[] = {IREE_SV("fill"), IREE_SV("add"),
                                      IREE_SV("@combined.alias[0]")};
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(names); ++i) {
    SCOPED_TRACE(std::string(names[i].data, names[i].size));
    iree_hal_executable_function_t function;
    IREE_ASSERT_OK(iree_hal_executable_lookup_function_by_name(
        executable_, names[i], &function));
    const auto correct_constants =
        iree_make_const_byte_span(constants, i * sizeof(int32_t));
    const auto wrong_constants =
        iree_make_const_byte_span(constants, ((i + 1) % 3) * sizeof(int32_t));
    const iree_hal_buffer_ref_list_t correct_bindings = {i + 1, bindings};
    const iree_hal_buffer_ref_list_t wrong_bindings = {(i + 1) % 3 + 1,
                                                       bindings};
    IREE_EXPECT_STATUS_IS(
        StatusCode::kInvalidArgument,
        iree_hal_queue_dispatch(
            dispatch_queue_, iree_hal_semaphore_list_empty(),
            iree_hal_semaphore_list_empty(), executable_, function,
            iree_hal_make_static_dispatch_config(1, 1, 1), wrong_constants,
            correct_bindings, IREE_HAL_DISPATCH_FLAG_NONE));
    IREE_EXPECT_STATUS_IS(
        StatusCode::kInvalidArgument,
        iree_hal_queue_dispatch(
            dispatch_queue_, iree_hal_semaphore_list_empty(),
            iree_hal_semaphore_list_empty(), executable_, function,
            iree_hal_make_static_dispatch_config(1, 1, 1), correct_constants,
            wrong_bindings, IREE_HAL_DISPATCH_FLAG_NONE));
  }
}

TEST_P(SpirvArtifactExecutionTest, RejectsMissingEntryMetadata) {
  const iree_file_toc_t* artifact = loom_spirv_multiple_entries_create();
  std::string bytes(artifact[0].data, artifact[0].size);
  const std::string prefix = "iree.vulkan.bda.v1[add]";
  size_t replaced = 0;
  for (size_t position = bytes.find(prefix); position != std::string::npos;
       position = bytes.find(prefix, position + prefix.size())) {
    // An unknown version is ignored, leaving only this entry without metadata.
    bytes[position + prefix.find("v1") + 1] = '2';
    ++replaced;
  }
  ASSERT_GT(replaced, 0u);
  const auto target =
      SelectExecutableTarget(IREE_SV("spirv"), IREE_SV("vulkan1.3+bda"));
  Ref<iree_hal_executable_t> incomplete;
  IREE_EXPECT_STATUS_IS(
      StatusCode::kInvalidArgument,
      LoadExecutable(iree_hal_queue_family(dispatch_queue_), target.target,
                     IREE_HAL_EXECUTABLE_LOAD_FLAG_NONE,
                     iree_make_const_byte_span(bytes.data(), bytes.size()),
                     incomplete.out()));
  EXPECT_EQ(incomplete.get(), nullptr);
}

CTS_REGISTER_TEST_SUITE(SpirvArtifactExecutionTest);

}  // namespace
}  // namespace iree::hal::cts
