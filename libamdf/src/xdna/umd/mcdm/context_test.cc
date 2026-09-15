// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/context.h"

#include <malloc.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"
#include "libamdf/src/xdna/endpoint_profile.h"
#include "libamdf/src/xdna/target/npu4/bootstrap.h"
#include "libamdf/src/xdna/target/npu5/bootstrap.h"
#include "libamdf/src/xdna/umd/mcdm/context.h"
#include "libamdf/src/xdna/umd/mcdm/device.h"

namespace {

constexpr NTSTATUS kSuccess = 0;
constexpr NTSTATUS kFailure = static_cast<NTSTATUS>(0xC0000001u);

enum class Operation {
  kNone,
  kCreateKernelBuffer,
  kMapKernelBuffer,
  kMakeKernelBufferResident,
  kLockKernelBuffer,
  kCreateContext,
  kDestroyContext,
  kUnlockKernelBuffer,
  kDestroyKernelBuffer,
  kFreeHostAllocation,
};

struct FakeKmtState {
  // Native status returned while qualifying the private context ABI.
  NTSTATUS query_status = kSuccess;
  // Standard KMD build identity; zero exercises private-query admission.
  uint64_t driver_version = 0;
  // Native reserved word and hardware kind, not an ABI tag.
  uint32_t private_info[2] = {0, 3};
  // Current protocol requires the expanded private-query record.
  bool current_protocol = false;
  // Current native allocation policy reported by the query.
  uint8_t unshared_kernel_buffers = 0;
  // Native dependency failure injected before the selected operation accepts
  // work.
  Operation failure = Operation::kNone;
  // Kernel buffer backing supplied by the native allocation dependency.
  alignas(4096) std::array<uint8_t, 4096> kernel_buffer = {};
  // Whether the native allocation is still owned.
  bool kernel_buffer_live = false;
  // Whether the native host lock is still owned.
  bool kernel_buffer_locked = false;
  // Number of context ABI queries.
  uint32_t query_count = 0;
  // Next context handle returned by native creation.
  D3DKMT_HANDLE next_context = 0x30;
  // Number of context destruction calls rejected before consumption.
  uint32_t destroy_failures_remaining = 0;
  // Context handles accepted by native creation.
  std::vector<D3DKMT_HANDLE> created_contexts;
  // Context handles consumed by native destruction.
  std::vector<D3DKMT_HANDLE> destroyed_contexts;
  // Ordered native and host releases observed during context teardown.
  std::vector<Operation> operations;
};

FakeKmtState* current_state = nullptr;

NTSTATUS APIENTRY FakeQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO* query) {
  ++current_state->query_count;
  EXPECT_EQ(query->hAdapter, 0x08u);
  if (current_state->query_status != kSuccess)
    return current_state->query_status;
  if (query->Type == KMTQAITYPE_KMD_DRIVER_VERSION) {
    EXPECT_EQ(query->PrivateDriverDataSize, sizeof(D3DKMT_KMD_DRIVER_VERSION));
    auto* version =
        static_cast<D3DKMT_KMD_DRIVER_VERSION*>(query->pPrivateDriverData);
    version->DriverVersion.QuadPart = current_state->driver_version;
    return kSuccess;
  }
  EXPECT_EQ(query->Type, KMTQAITYPE_UMDRIVERPRIVATE);
  if (current_state->current_protocol && query->PrivateDriverDataSize < 12) {
    return static_cast<NTSTATUS>(0xC0000023u);
  }
  std::memcpy(query->pPrivateDriverData, current_state->private_info,
              sizeof(current_state->private_info));
  if (current_state->current_protocol) {
    static_cast<uint8_t*>(query->pPrivateDriverData)[8] =
        current_state->unshared_kernel_buffers;
  }
  return kSuccess;
}

NTSTATUS APIENTRY
FakeCreateContextVirtual(D3DKMT_CREATECONTEXTVIRTUAL* create) {
  if (current_state->current_protocol) {
    current_state->operations.push_back(Operation::kCreateContext);
    EXPECT_TRUE(current_state->kernel_buffer_live);
    EXPECT_TRUE(current_state->kernel_buffer_locked);
    EXPECT_EQ(create->PrivateDriverDataSize, 160u);
    const auto* bytes = static_cast<const uint8_t*>(create->pPrivateDriverData);
    uint64_t allocation_handle = 0;
    uint64_t host_address = 0;
    uint32_t columns = 0;
    std::memcpy(&allocation_handle, bytes + 0x68, sizeof(allocation_handle));
    std::memcpy(&host_address, bytes + 0x78, sizeof(host_address));
    std::memcpy(&columns, bytes + 0x54, sizeof(columns));
    EXPECT_EQ(allocation_handle, 0x80u);
    EXPECT_EQ(host_address,
              reinterpret_cast<uintptr_t>(current_state->kernel_buffer.data()));
    EXPECT_EQ(columns, 1u);
    if (current_state->failure == Operation::kCreateContext) return kFailure;
  }
  EXPECT_EQ(create->hDevice, 0x10u);
  EXPECT_EQ(create->NodeOrdinal, 0u);
  EXPECT_EQ(create->EngineAffinity, 1u);
  EXPECT_EQ(create->Flags.HwQueueSupported, 1u);
  EXPECT_NE(create->pPrivateDriverData, nullptr);
  EXPECT_GT(create->PrivateDriverDataSize, 0u);
  create->hContext = current_state->next_context++;
  const uint32_t cookie = create->hContext - 0x30;
  const bool metadata =
      current_state->driver_version == UINT64_C(0x0020000000CB00F0);
  if (metadata) {
    EXPECT_EQ(create->PrivateDriverDataSize, 272u);
  }
  std::memcpy(static_cast<uint8_t*>(create->pPrivateDriverData) +
                  (metadata                          ? 0x30
                   : current_state->current_protocol ? 0x44
                                                     : 0x40),
              &cookie, sizeof(cookie));
  current_state->created_contexts.push_back(create->hContext);
  return kSuccess;
}

NTSTATUS APIENTRY FakeCreateAllocation(D3DKMT_CREATEALLOCATION* create) {
  current_state->operations.push_back(Operation::kCreateKernelBuffer);
  if (current_state->failure == Operation::kCreateKernelBuffer) return kFailure;
  EXPECT_FALSE(current_state->kernel_buffer_live);
  EXPECT_EQ(create->Flags.CreateShared,
            current_state->unshared_kernel_buffers == 0);
  EXPECT_EQ(create->Flags.CreateResource, create->Flags.CreateShared);
  create->pAllocationInfo2->hAllocation = 0x80;
  if (create->Flags.CreateResource) create->hResource = 0x81;
  current_state->kernel_buffer_live = true;
  return kSuccess;
}

NTSTATUS APIENTRY FakeMapAddress(D3DDDI_MAPGPUVIRTUALADDRESS* map) {
  current_state->operations.push_back(Operation::kMapKernelBuffer);
  if (current_state->failure == Operation::kMapKernelBuffer) return kFailure;
  EXPECT_TRUE(current_state->kernel_buffer_live);
  map->VirtualAddress = 0x100000000;
  return kSuccess;
}

NTSTATUS APIENTRY FakeMakeResident(D3DDDI_MAKERESIDENT*) {
  current_state->operations.push_back(Operation::kMakeKernelBufferResident);
  return current_state->failure == Operation::kMakeKernelBufferResident
             ? kFailure
             : kSuccess;
}

NTSTATUS APIENTRY FakeLock(D3DKMT_LOCK2* lock) {
  current_state->operations.push_back(Operation::kLockKernelBuffer);
  if (current_state->failure == Operation::kLockKernelBuffer) return kFailure;
  lock->pData = current_state->kernel_buffer.data();
  current_state->kernel_buffer_locked = true;
  return kSuccess;
}

NTSTATUS APIENTRY FakeUnlock(const D3DKMT_UNLOCK2*) {
  current_state->operations.push_back(Operation::kUnlockKernelBuffer);
  EXPECT_TRUE(current_state->kernel_buffer_locked);
  current_state->kernel_buffer_locked = false;
  return kSuccess;
}

NTSTATUS APIENTRY FakeDestroyAllocation(const D3DKMT_DESTROYALLOCATION2*) {
  current_state->operations.push_back(Operation::kDestroyKernelBuffer);
  EXPECT_TRUE(current_state->kernel_buffer_live);
  EXPECT_FALSE(current_state->kernel_buffer_locked);
  current_state->kernel_buffer_live = false;
  return kSuccess;
}

NTSTATUS APIENTRY FakeDestroyContext(const D3DKMT_DESTROYCONTEXT* destroy) {
  current_state->operations.push_back(Operation::kDestroyContext);
  if (current_state->destroy_failures_remaining != 0) {
    --current_state->destroy_failures_remaining;
    return kFailure;
  }
  current_state->destroyed_contexts.push_back(destroy->hContext);
  return kSuccess;
}

struct FaultAllocatorState {
  // One-based allocation call on which to report exhaustion.
  uint32_t failure_call = 0;
  // Number of allocation calls observed.
  uint32_t allocation_call_count = 0;
  // Number of allocations currently owned by the allocator.
  uint32_t live_allocation_count = 0;
  // Optional operation log receiving successful host releases.
  std::vector<Operation>* operations = nullptr;
};

void* AMDF_CALL FaultAllocate(void* user_data, uint64_t byte_length,
                              uint64_t minimum_alignment) {
  auto* state = static_cast<FaultAllocatorState*>(user_data);
  ++state->allocation_call_count;
  if (state->allocation_call_count == state->failure_call) return nullptr;
  void* pointer = _aligned_malloc(static_cast<size_t>(byte_length),
                                  static_cast<size_t>(minimum_alignment));
  if (pointer != nullptr) ++state->live_allocation_count;
  return pointer;
}

void AMDF_CALL FaultFree(void* user_data, void* allocation) {
  if (allocation == nullptr) return;
  auto* state = static_cast<FaultAllocatorState*>(user_data);
  EXPECT_NE(state->live_allocation_count, 0u);
  --state->live_allocation_count;
  if (state->operations != nullptr) {
    state->operations->push_back(Operation::kFreeHostAllocation);
  }
  _aligned_free(allocation);
}

class WindowsXdnaContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    current_state = &state_;
    kmt_.query_adapter_info = FakeQueryAdapterInfo;
    kmt_.create_context_virtual = FakeCreateContextVirtual;
    kmt_.destroy_context = FakeDestroyContext;
    kmt_.create_allocation = FakeCreateAllocation;
    kmt_.destroy_allocation = FakeDestroyAllocation;
    kmt_.map_gpu_virtual_address = FakeMapAddress;
    kmt_.make_resident = FakeMakeResident;
    kmt_.lock = FakeLock;
    kmt_.unlock = FakeUnlock;
    kmt_.wait_from_cpu =
        [](const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU*) -> NTSTATUS {
      ADD_FAILURE() << "Paging dependency completes synchronously";
      return kFailure;
    };
    device_.host_allocator = amdf_allocator_system();
    device_.kmt = &kmt_;
    device_.adapter = 0x08;
    device_.device = 0x10;
    endpoint_info_.array.column_count = 8;
    profile_.execution_capabilities =
        AMDF_XDNA_EXECUTION_CAPABILITY_TRANSACTION_INTERPRETER_V1;
    profile_.info = &endpoint_info_;
    profile_.bootstrap = &amdf_xdna_npu5_bootstrap;
    device_.profile = &profile_;
    create_info_.logical_column_count = 1;
    create_info_.physical_column_origin = AMDF_XDNA_PHYSICAL_COLUMN_ORIGIN_ANY;
    create_info_.acceptable_scheduling_modes =
        AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED;
  }

  void TearDown() override { current_state = nullptr; }

  FakeKmtState state_;
  amdf_kmt_api_t kmt_ = {};
  amdf_xdna_umd_device_t device_ = {};
  amdf_xdna_endpoint_info_t endpoint_info_ = {};
  amdf_xdna_endpoint_profile_t profile_ = {};
  amdf_xdna_context_create_info_t create_info_ = {};
};

TEST_F(WindowsXdnaContextTest, CreatesTwoContextsAndDestroysIndependently) {
  amdf_xdna_umd_context_t* contexts[2] = {};
  amdf_xdna_umd_context_result_t results[2] = {};
  for (size_t i = 0; i < 2; ++i) {
    ASSERT_EQ(amdf_xdna_umd_context_create(&device_, &create_info_,
                                           &contexts[i], &results[i]),
              AMDF_STATUS_OK);
    ASSERT_NE(contexts[i], nullptr);
    EXPECT_EQ(contexts[i]->device, &device_);
    EXPECT_EQ(results[i].physical_column_count, 0u);
  }
  EXPECT_NE(results[0].id.words[0], results[1].id.words[0]);
  EXPECT_EQ(state_.query_count, 4u);
  EXPECT_EQ(state_.created_contexts, (std::vector<D3DKMT_HANDLE>{0x30, 0x31}));

  ASSERT_EQ(amdf_xdna_umd_context_destroy(contexts[0]), AMDF_STATUS_OK);
  contexts[0] = nullptr;
  EXPECT_EQ(contexts[1]->handle, 0x31u);
  EXPECT_EQ(state_.destroyed_contexts, (std::vector<D3DKMT_HANDLE>{0x30}));
  ASSERT_EQ(amdf_xdna_umd_context_destroy(contexts[1]), AMDF_STATUS_OK);
  contexts[1] = nullptr;
  EXPECT_EQ(state_.destroyed_contexts,
            (std::vector<D3DKMT_HANDLE>{0x30, 0x31}));
}

TEST_F(WindowsXdnaContextTest, CreatesMetadataContextWithZeroCookie) {
  state_.driver_version = UINT64_C(0x0020000000CB00F0);
  profile_.bootstrap = &amdf_xdna_npu4_bootstrap;
  amdf_xdna_umd_context_t* context = nullptr;
  amdf_xdna_umd_context_result_t result = {};
  ASSERT_EQ(
      amdf_xdna_umd_context_create(&device_, &create_info_, &context, &result),
      AMDF_STATUS_OK);
  EXPECT_EQ(state_.query_count, 1u);
  EXPECT_EQ(context->command_aperture_cookie, 0u);
  EXPECT_EQ(context->native_abi.submission_header_byte_length, 88u);
  EXPECT_EQ(amdf_xdna_umd_context_destroy(context), AMDF_STATUS_OK);
  EXPECT_EQ(state_.destroyed_contexts, (std::vector<D3DKMT_HANDLE>{0x30}));
}

TEST_F(WindowsXdnaContextTest,
       RetainsCurrentKernelBufferUntilContextDestruction) {
  state_.current_protocol = true;
  for (uint8_t unshared : {uint8_t{0}, uint8_t{1}}) {
    state_.unshared_kernel_buffers = unshared;
    state_.operations.clear();
    amdf_xdna_umd_context_t* context = nullptr;
    amdf_xdna_umd_context_result_t result = {};
    ASSERT_EQ(amdf_xdna_umd_context_create(&device_, &create_info_, &context,
                                           &result),
              AMDF_STATUS_OK);
    EXPECT_EQ(state_.operations,
              (std::vector<Operation>{
                  Operation::kCreateKernelBuffer, Operation::kMapKernelBuffer,
                  Operation::kMakeKernelBufferResident,
                  Operation::kLockKernelBuffer, Operation::kCreateContext}));
    EXPECT_TRUE(state_.kernel_buffer_live);
    EXPECT_TRUE(state_.kernel_buffer_locked);
    state_.operations.clear();
    EXPECT_EQ(amdf_xdna_umd_context_destroy(context), AMDF_STATUS_OK);
    EXPECT_EQ(state_.operations,
              (std::vector<Operation>{Operation::kDestroyContext,
                                      Operation::kUnlockKernelBuffer,
                                      Operation::kDestroyKernelBuffer}));
    EXPECT_FALSE(state_.kernel_buffer_live);
    EXPECT_FALSE(state_.kernel_buffer_locked);
  }
}

TEST_F(WindowsXdnaContextTest, RollsBackCurrentKernelBufferBeforePublication) {
  state_.current_protocol = true;
  for (Operation failure :
       {Operation::kCreateKernelBuffer, Operation::kMapKernelBuffer,
        Operation::kMakeKernelBufferResident, Operation::kLockKernelBuffer,
        Operation::kCreateContext}) {
    state_.failure = failure;
    auto* const sentinel =
        reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
    amdf_xdna_umd_context_t* context = sentinel;
    amdf_xdna_umd_context_result_t result;
    std::memset(&result, 0xA5, sizeof(result));
    const auto original = result;
    EXPECT_EQ(amdf_xdna_umd_context_create(&device_, &create_info_, &context,
                                           &result),
              amdf_kmt_make_status(kFailure));
    EXPECT_EQ(context, sentinel);
    EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
    EXPECT_FALSE(state_.kernel_buffer_live);
    EXPECT_FALSE(state_.kernel_buffer_locked);
    EXPECT_TRUE(state_.created_contexts.empty());
  }
}

TEST_F(WindowsXdnaContextTest, ReleasesCurrentKernelBufferOnHostExhaustion) {
  state_.current_protocol = true;
  for (uint32_t failure_call : {1u, 2u, 3u}) {
    FaultAllocatorState allocator_state = {.failure_call = failure_call};
    device_.host_allocator = {
        .user_data = &allocator_state,
        .allocate = FaultAllocate,
        .free = FaultFree,
    };
    auto* const sentinel =
        reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
    amdf_xdna_umd_context_t* context = sentinel;
    amdf_xdna_umd_context_result_t result;
    std::memset(&result, 0xA5, sizeof(result));
    const auto original = result;
    EXPECT_EQ(amdf_status_code(amdf_xdna_umd_context_create(
                  &device_, &create_info_, &context, &result)),
              AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
    EXPECT_EQ(context, sentinel);
    EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
    EXPECT_EQ(allocator_state.live_allocation_count, 0u);
    EXPECT_FALSE(state_.kernel_buffer_live);
    EXPECT_FALSE(state_.kernel_buffer_locked);
    EXPECT_EQ(state_.destroyed_contexts, state_.created_contexts);
  }
}

TEST_F(WindowsXdnaContextTest,
       RetainsKernelBufferWhenNativeContextRemainsLive) {
  state_.current_protocol = true;
  amdf_xdna_umd_context_t* context = nullptr;
  amdf_xdna_umd_context_result_t result = {};
  ASSERT_EQ(
      amdf_xdna_umd_context_create(&device_, &create_info_, &context, &result),
      AMDF_STATUS_OK);
  state_.destroy_failures_remaining = 1;
  EXPECT_EQ(amdf_xdna_umd_context_destroy(context),
            amdf_kmt_make_status(kFailure));
  EXPECT_TRUE(state_.kernel_buffer_live);
  EXPECT_TRUE(state_.kernel_buffer_locked);
  EXPECT_TRUE(state_.destroyed_contexts.empty());
  EXPECT_EQ(amdf_xdna_umd_context_destroy(context), AMDF_STATUS_OK);
  EXPECT_FALSE(state_.kernel_buffer_live);
  EXPECT_FALSE(state_.kernel_buffer_locked);
  EXPECT_EQ(state_.destroyed_contexts, state_.created_contexts);
}

TEST_F(WindowsXdnaContextTest,
       RejectsUnrepresentableNativeCookieWithoutPublishing) {
  state_.driver_version = UINT64_C(0x0020000000CB00F0);
  state_.next_context = 0x130;
  profile_.bootstrap = &amdf_xdna_npu4_bootstrap;
  auto* const sentinel =
      reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
  amdf_xdna_umd_context_t* context = sentinel;
  amdf_xdna_umd_context_result_t result;
  std::memset(&result, 0xA5, sizeof(result));
  const auto original = result;
  EXPECT_EQ(amdf_status_code(amdf_xdna_umd_context_create(
                &device_, &create_info_, &context, &result)),
            AMDF_STATUS_CODE_INTERNAL);
  EXPECT_EQ(context, sentinel);
  EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
  EXPECT_EQ(state_.destroyed_contexts, (std::vector<D3DKMT_HANDLE>{0x130}));
}

TEST_F(WindowsXdnaContextTest,
       RejectsUnavailableInterpreterAndContextProceduresBeforePreparation) {
  FaultAllocatorState allocator_state = {};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  for (uint32_t unavailable : {0u, 1u, 2u}) {
    profile_.execution_capabilities =
        unavailable == 0
            ? 0
            : AMDF_XDNA_EXECUTION_CAPABILITY_TRANSACTION_INTERPRETER_V1;
    kmt_.create_context_virtual =
        unavailable == 1 ? nullptr : FakeCreateContextVirtual;
    kmt_.destroy_context = unavailable == 2 ? nullptr : FakeDestroyContext;
    auto* const sentinel =
        reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
    amdf_xdna_umd_context_t* context = sentinel;
    amdf_xdna_umd_context_result_t result;
    std::memset(&result, 0xA5, sizeof(result));
    const amdf_xdna_umd_context_result_t original = result;
    EXPECT_EQ(amdf_status_code(amdf_xdna_umd_context_create(
                  &device_, &create_info_, &context, &result)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(context, sentinel);
    EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
  }
  EXPECT_EQ(state_.query_count, 0u);
  EXPECT_EQ(allocator_state.allocation_call_count, 0u);
  EXPECT_TRUE(state_.created_contexts.empty());
}

TEST_F(WindowsXdnaContextTest, QualifiesPrivateAbiBeforeContextPreparation) {
  FaultAllocatorState allocator_state = {};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  for (uint32_t failure : {0u, 1u, 2u}) {
    state_.query_status = failure == 0 ? kFailure : kSuccess;
    state_.private_info[0] = failure == 1 ? 1 : 0;
    state_.private_info[1] = failure == 2 ? UINT32_MAX : 3;
    auto* const sentinel =
        reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
    amdf_xdna_umd_context_t* context = sentinel;
    amdf_xdna_umd_context_result_t result;
    std::memset(&result, 0xA5, sizeof(result));
    const amdf_xdna_umd_context_result_t original = result;
    EXPECT_EQ(amdf_xdna_umd_context_create(&device_, &create_info_, &context,
                                           &result),
              failure == 0
                  ? amdf_kmt_make_status(kFailure)
                  : amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
    EXPECT_EQ(context, sentinel);
    EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
  }
  EXPECT_EQ(state_.query_count, 5u);
  EXPECT_EQ(allocator_state.allocation_call_count, 0u);
  EXPECT_TRUE(state_.created_contexts.empty());
}

TEST_F(WindowsXdnaContextTest,
       RejectsExplicitPlacementBeforeNativeOrHostAllocation) {
  FaultAllocatorState allocator_state = {};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  for (uint32_t origin : {0u, 1u, 4u}) {
    create_info_.physical_column_origin = origin;
    auto* const sentinel =
        reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
    amdf_xdna_umd_context_t* context = sentinel;
    amdf_xdna_umd_context_result_t result;
    std::memset(&result, 0xA5, sizeof(result));
    const amdf_xdna_umd_context_result_t original_result = result;
    EXPECT_EQ(amdf_status_code(amdf_xdna_umd_context_create(
                  &device_, &create_info_, &context, &result)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(context, sentinel);
    EXPECT_EQ(std::memcmp(&result, &original_result, sizeof(result)), 0);
  }
  EXPECT_EQ(allocator_state.allocation_call_count, 0u);
  EXPECT_TRUE(state_.created_contexts.empty());
  EXPECT_TRUE(state_.destroyed_contexts.empty());
}

TEST_F(WindowsXdnaContextTest,
       DestroysNativeContextBeforePrivateExecutionStorage) {
  FaultAllocatorState allocator_state = {.operations = &state_.operations};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  amdf_xdna_umd_context_t* context = nullptr;
  amdf_xdna_umd_context_result_t result = {};
  ASSERT_EQ(
      amdf_xdna_umd_context_create(&device_, &create_info_, &context, &result),
      AMDF_STATUS_OK);
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(allocator_state.live_allocation_count, 2u);
  state_.operations.clear();

  ASSERT_EQ(amdf_xdna_umd_context_destroy(context), AMDF_STATUS_OK);

  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{Operation::kDestroyContext,
                                    Operation::kFreeHostAllocation,
                                    Operation::kFreeHostAllocation}));
  EXPECT_EQ(allocator_state.live_allocation_count, 0u);
}

TEST_F(WindowsXdnaContextTest, ReleasesFailedRollbackWithoutPublishingOutputs) {
  FaultAllocatorState allocator_state = {.failure_call = 3};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  state_.destroy_failures_remaining = 1;
  auto* const context_sentinel =
      reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
  amdf_xdna_umd_context_t* context = context_sentinel;
  amdf_xdna_umd_context_result_t result;
  std::memset(&result, 0xA5, sizeof(result));
  amdf_xdna_umd_context_result_t expected_result = result;

  EXPECT_EQ(
      amdf_xdna_umd_context_create(&device_, &create_info_, &context, &result),
      amdf_kmt_make_status(kFailure));
  EXPECT_EQ(context, context_sentinel);
  EXPECT_EQ(std::memcmp(&result, &expected_result, sizeof(result)), 0);
  EXPECT_EQ(allocator_state.live_allocation_count, 0u);
  EXPECT_TRUE(state_.destroyed_contexts.empty());
  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{Operation::kDestroyContext}));
}

TEST_F(WindowsXdnaContextTest, RollsBackHostExhaustionAtEachConstructionStep) {
  for (uint32_t failure_call : {1u, 2u, 3u}) {
    FaultAllocatorState allocator_state = {.failure_call = failure_call};
    device_.host_allocator = {
        .user_data = &allocator_state,
        .allocate = FaultAllocate,
        .free = FaultFree,
    };
    auto* const sentinel =
        reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
    amdf_xdna_umd_context_t* context = sentinel;
    amdf_xdna_umd_context_result_t result;
    std::memset(&result, 0xA5, sizeof(result));
    const auto original = result;

    EXPECT_EQ(amdf_status_code(amdf_xdna_umd_context_create(
                  &device_, &create_info_, &context, &result)),
              AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
    EXPECT_EQ(context, sentinel);
    EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
    EXPECT_EQ(allocator_state.live_allocation_count, 0u);
  }
  EXPECT_EQ(state_.created_contexts, (std::vector<D3DKMT_HANDLE>{0x30}));
  EXPECT_EQ(state_.destroyed_contexts, state_.created_contexts);
}

TEST_F(WindowsXdnaContextTest, ReportsMalformedContextRollbackFailureLocally) {
  FaultAllocatorState allocator_state = {};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  state_.driver_version = UINT64_C(0x0020000000CB00F0);
  state_.next_context = 0x130;
  state_.destroy_failures_remaining = 1;
  profile_.bootstrap = &amdf_xdna_npu4_bootstrap;
  auto* const sentinel =
      reinterpret_cast<amdf_xdna_umd_context_t*>(uintptr_t{1});
  amdf_xdna_umd_context_t* context = sentinel;
  amdf_xdna_umd_context_result_t result;
  std::memset(&result, 0xA5, sizeof(result));
  const auto original = result;

  EXPECT_EQ(
      amdf_xdna_umd_context_create(&device_, &create_info_, &context, &result),
      amdf_kmt_make_status(kFailure));
  EXPECT_EQ(context, sentinel);
  EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
  EXPECT_EQ(allocator_state.live_allocation_count, 0u);
  EXPECT_TRUE(state_.destroyed_contexts.empty());
  EXPECT_EQ(state_.operations,
            (std::vector<Operation>{Operation::kDestroyContext}));
}

TEST_F(WindowsXdnaContextTest, ExplicitDestroyFailureRetainsPublishedOwner) {
  FaultAllocatorState allocator_state = {};
  device_.host_allocator = {
      .user_data = &allocator_state,
      .allocate = FaultAllocate,
      .free = FaultFree,
  };
  amdf_xdna_umd_context_t* context = nullptr;
  amdf_xdna_umd_context_result_t result = {};
  ASSERT_EQ(
      amdf_xdna_umd_context_create(&device_, &create_info_, &context, &result),
      AMDF_STATUS_OK);
  state_.destroy_failures_remaining = 1;
  EXPECT_EQ(amdf_xdna_umd_context_destroy(context),
            amdf_kmt_make_status(kFailure));
  EXPECT_EQ(context->handle, 0x30u);
  EXPECT_NE(context->kernel_execution, nullptr);
  EXPECT_EQ(allocator_state.live_allocation_count, 2u);
  EXPECT_TRUE(state_.destroyed_contexts.empty());

  EXPECT_EQ(amdf_xdna_umd_context_destroy(context), AMDF_STATUS_OK);
  EXPECT_EQ(state_.destroyed_contexts, (std::vector<D3DKMT_HANDLE>{0x30}));
  EXPECT_EQ(allocator_state.live_allocation_count, 0u);
}

}  // namespace
