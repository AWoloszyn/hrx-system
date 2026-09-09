// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/memory.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"
#include "libamdf/src/xdna/umd/mcdm/device.h"

namespace {

constexpr NTSTATUS kStatusPending = static_cast<NTSTATUS>(0x00000103u);
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017u);

enum class FailurePoint {
  kNone,
  kCreate,
  kMap,
  kInvalidMapAddress,
  kFirstWait,
  kResident,
  kPartialResident,
  kSecondWait,
  kDestroy,
};

enum class Operation {
  kCreate,
  kMap,
  kWait,
  kResident,
  kDestroy,
};

struct FakeKmtState {
  // Native operation selected for a failure response.
  FailurePoint failure_point = FailurePoint::kNone;
  // Number of allocation release calls rejected before consuming the handle.
  uint32_t destroy_failures_remaining = 0;
  // Number of matching paging waits rejected before completion is observed.
  uint32_t wait_failures_remaining = 1;
  // Real host backing borrowed by the modeled native allocation.
  const void* host_pointer = nullptr;
  // Memory and host-view metadata returned to the host allocator.
  uint32_t metadata_free_count = 0;
  // Flags observed in the residency request.
  D3DDDI_MAKERESIDENT_FLAGS resident_flags = {};
  // Native operations in call order.
  std::vector<Operation> operations;
  // Paging fence values observed by CPU waits.
  std::vector<uint64_t> wait_targets;
};

FakeKmtState* g_fake_state = nullptr;

NTSTATUS APIENTRY FakeCreateAllocation(D3DKMT_CREATEALLOCATION* create) {
  g_fake_state->operations.push_back(Operation::kCreate);
  if (g_fake_state->failure_point == FailurePoint::kCreate) {
    return kStatusNoMemory;
  }
  EXPECT_EQ(create->Flags.StandardAllocation, 1u);
  EXPECT_EQ(create->Flags.ExistingSysMem, 1u);
  EXPECT_EQ(create->NumAllocations, 1u);
  EXPECT_NE(create->pAllocationInfo2[0].pSystemMem, nullptr);
  g_fake_state->host_pointer = create->pAllocationInfo2[0].pSystemMem;
  create->pAllocationInfo2[0].hAllocation = 0x20;
  return 0;
}

NTSTATUS APIENTRY
FakeDestroyAllocation(const D3DKMT_DESTROYALLOCATION2* destroy) {
  g_fake_state->operations.push_back(Operation::kDestroy);
  EXPECT_EQ(destroy->hDevice, 0x10u);
  EXPECT_EQ(destroy->hResource, 0u);
  EXPECT_EQ(destroy->AllocationCount, 1u);
  EXPECT_EQ(destroy->phAllocationList[0], 0x20u);
  EXPECT_EQ(destroy->Flags.AssumeNotInUse, 1u);
  if (g_fake_state->destroy_failures_remaining != 0) {
    --g_fake_state->destroy_failures_remaining;
    return kStatusNoMemory;
  }
  if (g_fake_state->failure_point == FailurePoint::kDestroy) {
    return kStatusNoMemory;
  }
  return 0;
}

NTSTATUS APIENTRY FakeMapGpuVirtualAddress(D3DDDI_MAPGPUVIRTUALADDRESS* map) {
  g_fake_state->operations.push_back(Operation::kMap);
  if (g_fake_state->failure_point == FailurePoint::kMap) {
    return kStatusNoMemory;
  }
  EXPECT_EQ(map->hPagingQueue, 0x30u);
  EXPECT_EQ(map->hAllocation, 0x20u);
  EXPECT_EQ(map->SizeInPages, 16u);
  EXPECT_EQ(map->Protection.Write, 1u);
  map->VirtualAddress =
      g_fake_state->failure_point == FailurePoint::kInvalidMapAddress
          ? UINT64_C(0x12340001)
          : UINT64_C(0x12340000);
  map->PagingFenceValue = 1;
  return kStatusPending;
}

NTSTATUS APIENTRY FakeMakeResident(D3DDDI_MAKERESIDENT* resident) {
  g_fake_state->operations.push_back(Operation::kResident);
  g_fake_state->resident_flags = resident->Flags;
  if (g_fake_state->failure_point == FailurePoint::kResident) {
    return kStatusNoMemory;
  }
  EXPECT_EQ(resident->hPagingQueue, 0x30u);
  EXPECT_EQ(resident->NumAllocations, 1u);
  EXPECT_EQ(resident->AllocationList[0], 0x20u);
  if (g_fake_state->failure_point == FailurePoint::kPartialResident) {
    resident->NumAllocations = 0;
  }
  resident->PagingFenceValue = 2;
  return kStatusPending;
}

NTSTATUS APIENTRY
FakeWaitFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* wait) {
  g_fake_state->operations.push_back(Operation::kWait);
  EXPECT_EQ(wait->hDevice, 0x10u);
  EXPECT_EQ(wait->ObjectCount, 1u);
  EXPECT_EQ(wait->ObjectHandleArray[0], 0x40u);
  const uint64_t target = wait->FenceValueArray[0];
  g_fake_state->wait_targets.push_back(target);
  const uint64_t failing_target =
      g_fake_state->failure_point == FailurePoint::kFirstWait    ? 1
      : g_fake_state->failure_point == FailurePoint::kSecondWait ? 2
                                                                 : 0;
  if (target == failing_target && g_fake_state->wait_failures_remaining != 0) {
    --g_fake_state->wait_failures_remaining;
    return kStatusNoMemory;
  }
  return 0;
}

class WindowsXdnaMemoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_fake_state = &state_;
    kmt_.create_allocation = FakeCreateAllocation;
    kmt_.destroy_allocation = FakeDestroyAllocation;
    kmt_.map_gpu_virtual_address = FakeMapGpuVirtualAddress;
    kmt_.make_resident = FakeMakeResident;
    kmt_.wait_from_cpu = FakeWaitFromCpu;
    device_.host_allocator = amdf_allocator_system();
    device_.host_allocator.user_data = &state_;
    device_.host_allocator.free = [](void* user_data, void* allocation) {
      ++static_cast<FakeKmtState*>(user_data)->metadata_free_count;
      amdf_free(amdf_allocator_system(), allocation);
    };
    device_.kmt = &kmt_;
    device_.device = 0x10;
    device_.paging_queue = 0x30;
    device_.paging_sync_object = 0x40;
    device_.paging_fence = &paging_fence_;

    create_info_.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create_info_.structure_size = sizeof(create_info_);
    create_info_.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    create_info_.required_flags =
        AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    create_info_.byte_length = 4097;
    create_info_.minimum_alignment = 4096;
  }

  void TearDown() override { g_fake_state = nullptr; }

  void ReleaseLeakedBacking() {
    MEMORY_BASIC_INFORMATION information = {};
    ASSERT_NE(
        VirtualQuery(state_.host_pointer, &information, sizeof(information)),
        0u);
    EXPECT_EQ(information.State, MEM_COMMIT);
    // No real native allocation exists in this model. Reclaim the known test
    // backing without retrying the failed native operation.
    EXPECT_TRUE(VirtualFree(information.AllocationBase, 0, MEM_RELEASE));
  }

  // Native dependency results and ordered observations.
  FakeKmtState state_;
  // Native procedures borrowed by the production memory implementation.
  amdf_kmt_api_t kmt_ = {};
  // Explicitly live device borrowed by the memory constructor.
  amdf_xdna_umd_device_t device_ = {};
  // Monitored fence exposed to the production paging wait.
  volatile uint64_t paging_fence_ = 0;
  // System-memory request for an unaligned logical byte length.
  amdf_memory_create_info_t create_info_ = {};
};

TEST_F(WindowsXdnaMemoryTest, PublishesOnlyAfterMapAndOrdinaryResidency) {
  amdf_xdna_umd_memory_t* memory = nullptr;
  amdf_xdna_umd_memory_result_t result = {};
  ASSERT_TRUE(amdf_status_is_ok(
      amdf_xdna_umd_memory_create(&device_, &create_info_, &memory, &result)));
  ASSERT_NE(memory, nullptr);

  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                    Operation::kWait, Operation::kResident,
                                    Operation::kWait}));
  EXPECT_EQ(state_.resident_flags.CantTrimFurther, 0u);
  EXPECT_EQ(state_.resident_flags.MustSucceed, 0u);
  EXPECT_EQ(state_.wait_targets, (std::vector<uint64_t>{1, 2}));
  EXPECT_EQ(result.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
  EXPECT_EQ(result.flags,
            AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
  EXPECT_EQ(result.byte_length, UINT64_C(65536));
  EXPECT_EQ(result.alignment, UINT64_C(65536));
  EXPECT_TRUE(amdf_physical_memory_id_is_valid(&result.physical_backing_id));
  EXPECT_EQ(result.device_address, UINT64_C(0x12340000));

  amdf_memory_map_info_t map_info = {};
  map_info.byte_offset = 32;
  map_info.byte_length = 4096;
  map_info.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  amdf_xdna_umd_host_mapping_t* mapping = nullptr;
  amdf_xdna_umd_host_mapping_result_t map_result = {};
  ASSERT_TRUE(amdf_status_is_ok(
      amdf_xdna_umd_memory_map(memory, &map_info, &mapping, &map_result)));
  ASSERT_NE(mapping, nullptr);
  EXPECT_NE(map_result.pointer, nullptr);
  EXPECT_EQ(map_result.byte_length, map_info.byte_length);
  EXPECT_EQ(map_result.cacheability, AMDF_HOST_CACHEABILITY_WRITE_BACK);
  EXPECT_EQ(map_result.cache_line_size, 64u);
  EXPECT_TRUE(amdf_status_is_ok(amdf_xdna_umd_host_mapping_cache_control(
      mapping, AMDF_HOST_CACHE_OPERATION_FLUSH, 0, map_result.byte_length)));
  EXPECT_TRUE(amdf_status_is_ok(amdf_xdna_umd_host_mapping_cache_control(
      mapping, AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0,
      map_result.byte_length)));
  EXPECT_TRUE(amdf_status_is_ok(amdf_xdna_umd_host_mapping_destroy(mapping)));

  EXPECT_TRUE(amdf_status_is_ok(amdf_xdna_umd_memory_destroy(memory)));
  EXPECT_EQ(state_.operations.back(), Operation::kDestroy);
}

TEST_F(WindowsXdnaMemoryTest, RejectsUnavailablePropertiesBeforeAllocation) {
  auto expect_unsupported = [&](const amdf_memory_create_info_t& create_info) {
    amdf_xdna_umd_memory_t* memory =
        reinterpret_cast<amdf_xdna_umd_memory_t*>(uintptr_t{1});
    amdf_xdna_umd_memory_result_t result = {};

    const amdf_status_t status =
        amdf_xdna_umd_memory_create(&device_, &create_info, &memory, &result);

    EXPECT_EQ(amdf_status_code(status), AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(memory), uintptr_t{1});
    EXPECT_TRUE(state_.operations.empty());
  };

  amdf_memory_create_info_t create_info = create_info_;
  create_info.required_flags |= AMDF_MEMORY_FLAG_EXECUTABLE;
  expect_unsupported(create_info);

  create_info = create_info_;
  create_info.memory_class = AMDF_MEMORY_CLASS_LOCAL;
  expect_unsupported(create_info);

  create_info = create_info_;
  create_info.minimum_alignment = UINT64_C(131072);
  expect_unsupported(create_info);
}

TEST_F(WindowsXdnaMemoryTest,
       ReclaimsEveryFailurePrefixAfterAcceptedPagingRetires) {
  for (FailurePoint failure_point :
       {FailurePoint::kCreate, FailurePoint::kMap,
        FailurePoint::kInvalidMapAddress, FailurePoint::kFirstWait,
        FailurePoint::kResident, FailurePoint::kPartialResident,
        FailurePoint::kSecondWait}) {
    state_ = {};
    state_.failure_point = failure_point;
    amdf_xdna_umd_memory_t* memory =
        reinterpret_cast<amdf_xdna_umd_memory_t*>(uintptr_t{1});
    amdf_xdna_umd_memory_result_t result = {};

    const amdf_status_t status =
        amdf_xdna_umd_memory_create(&device_, &create_info_, &memory, &result);

    EXPECT_FALSE(amdf_status_is_ok(status));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(memory), uintptr_t{1});
    EXPECT_EQ(state_.metadata_free_count, 1u);
    switch (failure_point) {
      case FailurePoint::kCreate:
        EXPECT_EQ(state_.operations,
                  (std::vector<Operation>{Operation::kCreate}));
        break;
      case FailurePoint::kMap:
        EXPECT_EQ(state_.operations,
                  (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                          Operation::kDestroy}));
        break;
      case FailurePoint::kInvalidMapAddress:
        EXPECT_EQ(
            state_.operations,
            (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                    Operation::kWait, Operation::kDestroy}));
        break;
      case FailurePoint::kFirstWait:
        EXPECT_EQ(state_.operations,
                  (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                          Operation::kWait, Operation::kWait,
                                          Operation::kDestroy}));
        EXPECT_EQ(state_.wait_targets, (std::vector<uint64_t>{1, 1}));
        break;
      case FailurePoint::kResident:
        EXPECT_EQ(state_.operations,
                  (std::vector<Operation>{
                      Operation::kCreate, Operation::kMap, Operation::kWait,
                      Operation::kResident, Operation::kDestroy}));
        break;
      case FailurePoint::kPartialResident:
        EXPECT_EQ(
            state_.operations,
            (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                    Operation::kWait, Operation::kResident,
                                    Operation::kWait, Operation::kDestroy}));
        break;
      case FailurePoint::kSecondWait:
        EXPECT_EQ(state_.operations,
                  (std::vector<Operation>{
                      Operation::kCreate, Operation::kMap, Operation::kWait,
                      Operation::kResident, Operation::kWait, Operation::kWait,
                      Operation::kDestroy}));
        EXPECT_EQ(state_.wait_targets, (std::vector<uint64_t>{1, 2, 2}));
        break;
      case FailurePoint::kDestroy:
      case FailurePoint::kNone:
        FAIL() << "unexpected failure point";
        break;
    }
  }
}

TEST_F(WindowsXdnaMemoryTest,
       LeaksBackingWhenRollbackCannotObservePagingCompletion) {
  state_.failure_point = FailurePoint::kFirstWait;
  state_.wait_failures_remaining = 2;
  amdf_xdna_umd_memory_t* memory =
      reinterpret_cast<amdf_xdna_umd_memory_t*>(uintptr_t{1});
  amdf_xdna_umd_memory_result_t result;
  std::memset(&result, 0xA5, sizeof(result));
  const amdf_xdna_umd_memory_result_t original_result = result;

  EXPECT_EQ(
      amdf_xdna_umd_memory_create(&device_, &create_info_, &memory, &result),
      amdf_kmt_make_status(kStatusNoMemory));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(memory), uintptr_t{1});
  EXPECT_EQ(std::memcmp(&result, &original_result, sizeof(result)), 0);
  EXPECT_EQ(state_.metadata_free_count, 1u);
  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                    Operation::kWait, Operation::kWait}));

  EXPECT_EQ(state_.wait_targets, (std::vector<uint64_t>{1, 1}));
  ReleaseLeakedBacking();
}

TEST_F(WindowsXdnaMemoryTest, KeepsPublishedMemoryLiveAfterDestroyFailure) {
  amdf_xdna_umd_memory_t* memory = nullptr;
  amdf_xdna_umd_memory_result_t result = {};
  ASSERT_TRUE(amdf_status_is_ok(
      amdf_xdna_umd_memory_create(&device_, &create_info_, &memory, &result)));
  ASSERT_NE(memory, nullptr);

  state_.failure_point = FailurePoint::kDestroy;
  EXPECT_FALSE(amdf_status_is_ok(amdf_xdna_umd_memory_destroy(memory)));
  EXPECT_EQ(state_.metadata_free_count, 0u);
  state_.failure_point = FailurePoint::kNone;
  EXPECT_TRUE(amdf_status_is_ok(amdf_xdna_umd_memory_destroy(memory)));
  EXPECT_EQ(state_.metadata_free_count, 1u);
}

TEST_F(WindowsXdnaMemoryTest,
       ReportsFailedNativeCleanupWithoutRetainingMemory) {
  state_.failure_point = FailurePoint::kMap;
  state_.destroy_failures_remaining = 1;
  amdf_xdna_umd_memory_t* memory =
      reinterpret_cast<amdf_xdna_umd_memory_t*>(uintptr_t{1});
  amdf_xdna_umd_memory_result_t result;
  std::memset(&result, 0xA5, sizeof(result));
  const amdf_xdna_umd_memory_result_t original_result = result;

  EXPECT_EQ(
      amdf_xdna_umd_memory_create(&device_, &create_info_, &memory, &result),
      amdf_kmt_make_status(kStatusNoMemory));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(memory), uintptr_t{1});
  EXPECT_EQ(std::memcmp(&result, &original_result, sizeof(result)), 0);
  EXPECT_EQ(state_.metadata_free_count, 1u);
  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{Operation::kCreate, Operation::kMap,
                                    Operation::kDestroy}));
  ReleaseLeakedBacking();
}

}  // namespace
