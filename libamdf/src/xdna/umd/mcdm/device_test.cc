// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/device.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/src/platform/windows/endpoint.h"
#include "libamdf/src/xdna/endpoint_profile.h"

namespace {

constexpr NTSTATUS kSuccess = 0;
constexpr NTSTATUS kFailure = static_cast<NTSTATUS>(0xC0000001u);

enum class Operation {
  kQueryAdapter,
  kCreateDevice,
  kCreatePagingQueue,
  kDestroyPagingQueue,
  kDestroyDevice,
  kCloseAdapter,
};

struct FakeKmtState {
  // Number of paging-queue releases rejected before native consumption.
  uint32_t paging_queue_destroy_failures_remaining = 2;
  // Ordered native operations used to verify local and published ownership.
  std::vector<Operation> operations;
  // Number of successfully released paging queues.
  uint32_t paging_queue_destroy_success_count = 0;
  // Number of successfully released logical devices.
  uint32_t device_destroy_success_count = 0;
  // Number of successfully closed endpoint adapters.
  uint32_t adapter_close_success_count = 0;
  // Mapped paging progress returned with the malformed queue result.
  volatile uint64_t paging_progress = 0;
};

FakeKmtState* current_state = nullptr;

NTSTATUS APIENTRY FakeQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO* query) {
  current_state->operations.push_back(Operation::kQueryAdapter);
  EXPECT_EQ(query->Type, KMTQAITYPE_UMDRIVERPRIVATE);
  EXPECT_EQ(query->PrivateDriverDataSize, sizeof(uint32_t) * 2);
  auto* private_info = static_cast<uint32_t*>(query->pPrivateDriverData);
  private_info[0] = 0;
  private_info[1] = 3;
  return kSuccess;
}

NTSTATUS APIENTRY FakeCreateDevice(D3DKMT_CREATEDEVICE* create) {
  current_state->operations.push_back(Operation::kCreateDevice);
  EXPECT_EQ(create->hAdapter, 0x08u);
  create->hDevice = 0x10;
  return kSuccess;
}

NTSTATUS APIENTRY FakeDestroyDevice(const D3DKMT_DESTROYDEVICE* destroy) {
  current_state->operations.push_back(Operation::kDestroyDevice);
  EXPECT_EQ(destroy->hDevice, 0x10u);
  ++current_state->device_destroy_success_count;
  return kSuccess;
}

NTSTATUS APIENTRY FakeGetDeviceState(D3DKMT_GETDEVICESTATE*) {
  ADD_FAILURE() << "device-state query is not part of this construction path";
  return kFailure;
}

NTSTATUS APIENTRY FakeCreatePagingQueue(D3DKMT_CREATEPAGINGQUEUE* create) {
  current_state->operations.push_back(Operation::kCreatePagingQueue);
  EXPECT_EQ(create->hDevice, 0x10u);
  create->hPagingQueue = 0x20;
  create->hSyncObject = 0;
  create->FenceValueCPUVirtualAddress =
      const_cast<uint64_t*>(&current_state->paging_progress);
  return kSuccess;
}

NTSTATUS APIENTRY FakeDestroyPagingQueue(D3DDDI_DESTROYPAGINGQUEUE* destroy) {
  current_state->operations.push_back(Operation::kDestroyPagingQueue);
  EXPECT_EQ(destroy->hPagingQueue, 0x20u);
  if (current_state->paging_queue_destroy_failures_remaining != 0) {
    --current_state->paging_queue_destroy_failures_remaining;
    return kFailure;
  }
  ++current_state->paging_queue_destroy_success_count;
  return kSuccess;
}

NTSTATUS APIENTRY FakeCreateContextVirtual(D3DKMT_CREATECONTEXTVIRTUAL*) {
  ADD_FAILURE() << "malformed paging state must stop context construction";
  return kFailure;
}

NTSTATUS APIENTRY FakeDestroyContext(const D3DKMT_DESTROYCONTEXT*) {
  ADD_FAILURE() << "no context was acquired by this construction path";
  return kFailure;
}

NTSTATUS APIENTRY FakeCloseAdapter(const D3DKMT_CLOSEADAPTER* close) {
  current_state->operations.push_back(Operation::kCloseAdapter);
  EXPECT_EQ(close->hAdapter, 0x08u);
  ++current_state->adapter_close_success_count;
  return kSuccess;
}

class WindowsXdnaDeviceRollbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    current_state = &state_;
    instance_.kmt.query_adapter_info = FakeQueryAdapterInfo;
    instance_.kmt.create_device = FakeCreateDevice;
    instance_.kmt.destroy_device = FakeDestroyDevice;
    instance_.kmt.get_device_state = FakeGetDeviceState;
    instance_.kmt.create_paging_queue = FakeCreatePagingQueue;
    instance_.kmt.destroy_paging_queue = FakeDestroyPagingQueue;
    instance_.kmt.create_context_virtual = FakeCreateContextVirtual;
    instance_.kmt.destroy_context = FakeDestroyContext;
    instance_.kmt.close_adapter = FakeCloseAdapter;

    endpoint_ = static_cast<amdf_platform_endpoint_t*>(
        std::calloc(1, sizeof(*endpoint_)));
    ASSERT_NE(endpoint_, nullptr);
    endpoint_->instance = &instance_;
    endpoint_->adapter = 0x08;
    endpoint_->physical_adapter_index = 0;

    endpoint_info_.array.column_origin = 0;
    endpoint_info_.array.column_count = 8;
    profile_.model = AMDF_PCI_XDNA_MODEL_NPU5;
    profile_.info = &endpoint_info_;

    create_info_.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_CREATE_INFO;
    create_info_.structure_size = sizeof(create_info_);
    create_info_.logical_column_count = 1;
    create_info_.physical_column_origin = AMDF_XDNA_PHYSICAL_COLUMN_ORIGIN_ANY;
    create_info_.acceptable_scheduling_modes =
        AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED;
  }

  void TearDown() override {
    state_.paging_queue_destroy_failures_remaining = 0;
    if (endpoint_ != nullptr) {
      EXPECT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
      endpoint_ = nullptr;
    }
    current_state = nullptr;
  }

  FakeKmtState state_;
  amdf_platform_instance_t instance_ = {};
  amdf_platform_endpoint_t* endpoint_ = nullptr;
  amdf_xdna_endpoint_info_t endpoint_info_ = {};
  amdf_xdna_endpoint_profile_t profile_ = {};
  amdf_xdna_device_create_info_t create_info_ = {};
};

TEST_F(WindowsXdnaDeviceRollbackTest,
       ReportsFailedRollbackWithoutRetainingDevice) {
  amdf_xdna_umd_device_t* device =
      reinterpret_cast<amdf_xdna_umd_device_t*>(uintptr_t{1});
  amdf_xdna_umd_device_result_t result = {};

  const amdf_status_t status = amdf_xdna_umd_device_create(
      endpoint_, &profile_, &create_info_, &device, &result);

  EXPECT_EQ(status, amdf_kmt_make_status(kFailure));
  EXPECT_EQ(device, nullptr);
  EXPECT_EQ(
      state_.operations,
      (std::vector<Operation>{
          Operation::kQueryAdapter, Operation::kCreateDevice,
          Operation::kCreatePagingQueue, Operation::kDestroyPagingQueue}));
  EXPECT_EQ(state_.paging_queue_destroy_success_count, 0u);
  EXPECT_EQ(state_.device_destroy_success_count, 0u);
  EXPECT_EQ(state_.adapter_close_success_count, 0u);

  EXPECT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
  endpoint_ = nullptr;
  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{
                Operation::kQueryAdapter, Operation::kCreateDevice,
                Operation::kCreatePagingQueue, Operation::kDestroyPagingQueue,
                Operation::kCloseAdapter}));
  EXPECT_EQ(state_.paging_queue_destroy_success_count, 0u);
  EXPECT_EQ(state_.device_destroy_success_count, 0u);
  EXPECT_EQ(state_.adapter_close_success_count, 1u);
}

}  // namespace
