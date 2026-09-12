// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/target/gfx1151/user_queue_memory_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "amdf/amdf.h"
#include "amdf/gpu.h"
#include "gtest/gtest.h"
#include "libamdf/cts/gpu/gpu_device_fixture.h"

namespace {

constexpr size_t kElementCount = 16;
constexpr uint64_t kMemoryByteLength = 4096;
constexpr uint64_t kCompletionByteOffset = 256;
constexpr uint32_t kCompletionValue = UINT32_C(0x71c04a5e);
constexpr size_t kPm4PublishedDwordCount = 128;
constexpr size_t kSdmaPublishedDwordCount = 22;
constexpr amdf_queue_roles_t kRequiredQueueRoles =
    AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL;

struct EncodedQueueStream {
  // Number of initialized command bytes in the ring.
  size_t byte_length;
  // Producer frontier in the units defined by the queue format.
  uint64_t published_index;
};

uint32_t MakePm4Header(uint32_t opcode, uint32_t dword_count) {
  return (UINT32_C(3) << 30) | (opcode << 8) | ((dword_count - 2) << 16);
}

void AppendSystemBarrier(uint32_t* words, size_t* ordinal) {
  enum : uint32_t {
    kEventWriteOpcode = 0x46,
    kAcquireMemoryOpcode = 0x58,
    kEventWriteDwordCount = 2,
    kAcquireMemoryDwordCount = 8,
    kComputeShaderPartialFlush = 7 | (4 << 8),
    kConservativeGcrControl = (3 << 0) | (1 << 4) | (1 << 5) | (1 << 7) |
                              (1 << 8) | (1 << 9) | (1 << 14) | (1 << 15),
  };
  words[(*ordinal)++] = MakePm4Header(kEventWriteOpcode, kEventWriteDwordCount);
  words[(*ordinal)++] = kComputeShaderPartialFlush;
  words[(*ordinal)++] =
      MakePm4Header(kAcquireMemoryOpcode, kAcquireMemoryDwordCount);
  words[(*ordinal)++] = 0;
  words[(*ordinal)++] = UINT32_MAX;
  words[(*ordinal)++] = 0xff;
  words[(*ordinal)++] = 0;
  words[(*ordinal)++] = 0;
  words[(*ordinal)++] = 0x0a;
  words[(*ordinal)++] = kConservativeGcrControl;
}

void AppendCopyData32(uint32_t* words, size_t* ordinal, uint64_t source_address,
                      uint64_t target_address) {
  enum : uint32_t {
    kCopyDataOpcode = 0x40,
    kCopyDataDwordCount = 6,
    kSourceTcL2 = 2 << 0,
    kTargetTcL2 = 2 << 8,
    kWaitForConfirmation = 1 << 20,
  };
  words[(*ordinal)++] = MakePm4Header(kCopyDataOpcode, kCopyDataDwordCount);
  words[(*ordinal)++] = kSourceTcL2 | kTargetTcL2 | kWaitForConfirmation;
  words[(*ordinal)++] =
      static_cast<uint32_t>(source_address) & UINT32_C(0xfffffffc);
  words[(*ordinal)++] = static_cast<uint32_t>(source_address >> 32);
  words[(*ordinal)++] =
      static_cast<uint32_t>(target_address) & UINT32_C(0xfffffffc);
  words[(*ordinal)++] = static_cast<uint32_t>(target_address >> 32);
}

void AppendWriteData32(uint32_t* words, size_t* ordinal,
                       uint64_t target_address, uint32_t value) {
  enum : uint32_t {
    kWriteDataOpcode = 0x37,
    kWriteDataDwordCount = 5,
    kTargetTcL2 = 2 << 8,
    kWaitForConfirmation = 1 << 20,
  };
  words[(*ordinal)++] = MakePm4Header(kWriteDataOpcode, kWriteDataDwordCount);
  words[(*ordinal)++] = kTargetTcL2 | kWaitForConfirmation;
  words[(*ordinal)++] =
      static_cast<uint32_t>(target_address) & UINT32_C(0xfffffffc);
  words[(*ordinal)++] = static_cast<uint32_t>(target_address >> 32);
  words[(*ordinal)++] = value;
}

size_t EncodePm4CopyStream(uint32_t* words, uint64_t source_address,
                           uint64_t target_address) {
  size_t ordinal = 0;
  AppendSystemBarrier(words, &ordinal);
  for (size_t i = 0; i < kElementCount; ++i) {
    AppendCopyData32(words, &ordinal, source_address + i * sizeof(uint32_t),
                     target_address + i * sizeof(uint32_t));
  }
  AppendSystemBarrier(words, &ordinal);
  AppendWriteData32(words, &ordinal, target_address + kCompletionByteOffset,
                    kCompletionValue);

  size_t padding_dword_count = 8 - ordinal % 8;
  if (padding_dword_count == 1) padding_dword_count += 8;
  words[ordinal] = MakePm4Header(0x10, padding_dword_count);
  std::memset(words + ordinal + 1, 0,
              (padding_dword_count - 1) * sizeof(*words));
  return ordinal + padding_dword_count;
}

void AppendSdmaCacheTransition(uint32_t* words, size_t* ordinal,
                               uint32_t control) {
  words[(*ordinal)++] = 17;
  words[(*ordinal)++] = 0;
  words[(*ordinal)++] = (control & UINT32_C(0xffff)) << 16;
  words[(*ordinal)++] = control >> 16;
  words[(*ordinal)++] = 0;
}

size_t EncodeSdmaCopyStream(uint32_t* words, uint64_t source_address,
                            uint64_t target_address) {
  enum : uint32_t {
    kAcquireControl = 0x043a1,
    kReleaseControl = 0x0c3a1,
    kCopyByteLength = kElementCount * sizeof(uint32_t),
    kUncachedFenceHeader = 5 | (3 << 16),
  };
  size_t ordinal = 0;
  AppendSdmaCacheTransition(words, &ordinal, kAcquireControl);
  words[ordinal++] = 1;
  words[ordinal++] = kCopyByteLength - 1;
  words[ordinal++] = 0;
  words[ordinal++] = static_cast<uint32_t>(source_address);
  words[ordinal++] = static_cast<uint32_t>(source_address >> 32);
  words[ordinal++] = static_cast<uint32_t>(target_address);
  words[ordinal++] = static_cast<uint32_t>(target_address >> 32);
  AppendSdmaCacheTransition(words, &ordinal, kReleaseControl);
  const uint64_t completion_address = target_address + kCompletionByteOffset;
  words[ordinal++] = kUncachedFenceHeader;
  words[ordinal++] = static_cast<uint32_t>(completion_address);
  words[ordinal++] = static_cast<uint32_t>(completion_address >> 32);
  words[ordinal++] = kCompletionValue;
  words[ordinal++] = 0;
  return ordinal;
}

EncodedQueueStream EncodeCopyStream(amdf_queue_command_type_t command_type,
                                    uint32_t* words, uint64_t source_address,
                                    uint64_t target_address) {
  if (command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_PM4) {
    const size_t dword_count =
        EncodePm4CopyStream(words, source_address, target_address);
    return {
        .byte_length = dword_count * sizeof(uint32_t),
        .published_index = dword_count,
    };
  }
  const size_t dword_count =
      EncodeSdmaCopyStream(words, source_address, target_address);
  return {
      .byte_length = dword_count * sizeof(uint32_t),
      .published_index = dword_count * sizeof(uint32_t),
  };
}

uint32_t QueueFormatVersion(amdf_queue_command_type_t command_type) {
  return command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_PM4
             ? AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1
             : AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1;
}

class UserQueueMemoryScenario {
 public:
  UserQueueMemoryScenario(const amdf_api_t* api, const amdf_gpu_api_t* gpu_api,
                          amdf_endpoint_t* endpoint, amdf_device_t* device)
      : api_(api), gpu_api_(gpu_api), endpoint_(endpoint), device_(device) {}

  void RunCopiesBetweenExactAccessAttachments(
      amdf_queue_command_type_t command_type);

  bool Release() {
    if (queue_mapping_ != nullptr) {
      const amdf_status_t status =
          api_->user_queue_mapping_destroy(queue_mapping_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) queue_mapping_ = nullptr;
    }
    if (queue_mapping_ != nullptr) return false;
    if (queue_ != nullptr) {
      const amdf_status_t status = api_->user_queue_destroy(queue_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) queue_ = nullptr;
    }
    // Workload memory remains attached while native execution may still reach
    // it. A queue that cannot prove destruction retains the complete fixture.
    if (queue_ != nullptr) return false;

    DestroyHostMapping(source_mapping_);
    DestroyHostMapping(target_mapping_);
    if (source_mapping_ == nullptr) DestroyMemory(source_memory_);
    if (target_mapping_ == nullptr) DestroyMemory(target_memory_);
    return source_memory_ == nullptr && target_memory_ == nullptr;
  }

 private:
  amdf_status_t FindTransferFamily(amdf_queue_command_type_t command_type,
                                   uint32_t* out_ordinal) const {
    amdf_endpoint_info_t endpoint_info = {};
    endpoint_info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
    endpoint_info.structure_size = sizeof(endpoint_info);
    amdf_status_t status = api_->endpoint_query_info(endpoint_, &endpoint_info);
    if (!amdf_status_is_ok(status)) return status;
    for (uint32_t ordinal = 0; ordinal < endpoint_info.queue_family_count;
         ++ordinal) {
      amdf_queue_family_info_t family = {};
      family.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
      family.structure_size = sizeof(family);
      status =
          api_->endpoint_query_queue_family_info(endpoint_, ordinal, &family);
      if (!amdf_status_is_ok(status)) return status;
      if (family.command_type == command_type &&
          family.format_version == QueueFormatVersion(command_type) &&
          (family.publication_modes & AMDF_QUEUE_PUBLICATION_MODE_USER) != 0 &&
          (family.roles & kRequiredQueueRoles) == kRequiredQueueRoles &&
          (family.user_queue_capabilities &
           AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER) != 0 &&
          (family.producer_modes & AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE) != 0 &&
          (family.priority_capabilities &
           AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL) != 0) {
        *out_ordinal = ordinal;
        return AMDF_STATUS_OK;
      }
    }
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  void CreateMappedSystemMemory(amdf_memory_access_t device_access,
                                amdf_memory_t*& memory,
                                amdf_memory_info_t& memory_info,
                                amdf_host_mapping_t*& mapping,
                                amdf_host_mapping_info_t& mapping_info) {
    constexpr amdf_memory_flags_t kRequiredFlags =
        AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_SHAREABLE |
        AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    const uint32_t profile_ordinal = FindGpuMemoryProfileOrdinal(
        api_, device_, AMDF_MEMORY_CLASS_SYSTEM,
        AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
        kRequiredFlags, device_access);
    ASSERT_NE(profile_ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);

    amdf_memory_create_info_t create_info = {};
    create_info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create_info.structure_size = sizeof(create_info);
    create_info.memory_profile_ordinal = profile_ordinal;
    create_info.device_access = device_access;
    create_info.required_flags = kRequiredFlags;
    create_info.byte_length = kMemoryByteLength;
    create_info.minimum_alignment = 4096;
    ASSERT_EQ(api_->memory_create(device_, &create_info, &memory),
              AMDF_STATUS_OK);

    memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
    memory_info.structure_size = sizeof(memory_info);
    ASSERT_EQ(api_->memory_query_info(memory, &memory_info), AMDF_STATUS_OK);
    EXPECT_EQ(memory_info.memory_profile_ordinal, profile_ordinal);
    EXPECT_EQ(memory_info.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
    EXPECT_EQ(memory_info.device_access, device_access);
    EXPECT_EQ(memory_info.flags & kRequiredFlags, kRequiredFlags);
    EXPECT_EQ(memory_info.byte_length, kMemoryByteLength);
    EXPECT_EQ(memory_info.native_allocation_byte_length, kMemoryByteLength);
    EXPECT_GE(memory_info.alignment, create_info.minimum_alignment);
    EXPECT_NE(memory_info.device_address, 0u);
    EXPECT_EQ(memory_info.device_address & (sizeof(uint32_t) - 1), 0u);
    EXPECT_NE(memory_info.physical_backing_id.words[0] |
                  memory_info.physical_backing_id.words[1],
              0u);

    amdf_memory_map_info_t map_info = {};
    map_info.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
    map_info.structure_size = sizeof(map_info);
    map_info.byte_length = kMemoryByteLength;
    map_info.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
    ASSERT_EQ(api_->memory_map(memory, &map_info, &mapping), AMDF_STATUS_OK);
    mapping_info.type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    mapping_info.structure_size = sizeof(mapping_info);
    ASSERT_EQ(api_->host_mapping_query_info(mapping, &mapping_info),
              AMDF_STATUS_OK);
    ASSERT_NE(mapping_info.pointer, nullptr);
    EXPECT_EQ(mapping_info.memory_byte_offset, 0u);
    EXPECT_EQ(mapping_info.byte_length, kMemoryByteLength);
    EXPECT_EQ(mapping_info.flags,
              AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE);
    EXPECT_EQ(mapping_info.cacheability, AMDF_HOST_CACHEABILITY_COHERENT);
    EXPECT_EQ(mapping_info.reset_epoch, memory_info.reset_epoch);
  }

  void DestroyHostMapping(amdf_host_mapping_t*& mapping) {
    if (mapping == nullptr) return;
    const amdf_status_t status = api_->host_mapping_destroy(mapping);
    EXPECT_EQ(status, AMDF_STATUS_OK);
    if (amdf_status_is_ok(status)) mapping = nullptr;
  }

  void DestroyMemory(amdf_memory_t*& memory) {
    if (memory == nullptr) return;
    const amdf_status_t status = api_->memory_destroy(memory);
    EXPECT_EQ(status, AMDF_STATUS_OK);
    if (amdf_status_is_ok(status)) memory = nullptr;
  }

  // Core API table borrowed from the enclosing device fixture.
  const amdf_api_t* api_;
  // GPU extension table borrowed from the enclosing device fixture.
  const amdf_gpu_api_t* gpu_api_;
  // Endpoint borrowed for queue-family discovery.
  amdf_endpoint_t* endpoint_;
  // Execution owner borrowed through the release of all children below.
  amdf_device_t* device_;
  // GPU-readable source attachment retained through queue destruction.
  amdf_memory_t* source_memory_ = nullptr;
  // GPU-writable target attachment retained through queue destruction.
  amdf_memory_t* target_memory_ = nullptr;
  // Host view of the source attachment.
  amdf_host_mapping_t* source_mapping_ = nullptr;
  // Host view of the target attachment.
  amdf_host_mapping_t* target_mapping_ = nullptr;
  // Directly published GPU queue.
  amdf_user_queue_t* queue_ = nullptr;
  // Host producer mapping of the GPU queue.
  amdf_user_queue_mapping_t* queue_mapping_ = nullptr;
  // Immutable properties of the source attachment.
  amdf_memory_info_t source_memory_info_ = {};
  // Immutable properties of the target attachment.
  amdf_memory_info_t target_memory_info_ = {};
  // Host-view properties of the source attachment.
  amdf_host_mapping_info_t source_mapping_info_ = {};
  // Host-view properties of the target attachment.
  amdf_host_mapping_info_t target_mapping_info_ = {};
};

void UserQueueMemoryScenario::RunCopiesBetweenExactAccessAttachments(
    amdf_queue_command_type_t command_type) {
  uint32_t queue_family_ordinal = UINT32_MAX;
  ASSERT_EQ(FindTransferFamily(command_type, &queue_family_ordinal),
            AMDF_STATUS_OK);
  ASSERT_NE(queue_family_ordinal, UINT32_MAX);

  ASSERT_NO_FATAL_FAILURE(CreateMappedSystemMemory(
      AMDF_MEMORY_ACCESS_READ, source_memory_, source_memory_info_,
      source_mapping_, source_mapping_info_));
  ASSERT_NO_FATAL_FAILURE(CreateMappedSystemMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, target_memory_,
      target_memory_info_, target_mapping_, target_mapping_info_));
  EXPECT_NE(source_memory_info_.device_address,
            target_memory_info_.device_address);
  EXPECT_FALSE(amdf_physical_memory_id_is_equal(
      &source_memory_info_.physical_backing_id,
      &target_memory_info_.physical_backing_id));

  auto* source = static_cast<uint32_t*>(source_mapping_info_.pointer);
  auto* target = static_cast<uint32_t*>(target_mapping_info_.pointer);
  auto* completion = reinterpret_cast<uint32_t*>(
      static_cast<uint8_t*>(target_mapping_info_.pointer) +
      kCompletionByteOffset);
  std::array<uint32_t, kElementCount> expected = {};
  for (size_t i = 0; i < kElementCount; ++i) {
    expected[i] =
        UINT32_C(0x13570000) + static_cast<uint32_t>(i) * UINT32_C(0x00110101);
    source[i] = expected[i];
    target[i] = UINT32_C(0xdeadbeef);
  }
  *completion = 0;
  ASSERT_EQ(api_->host_mapping_cache_control(source_mapping_,
                                             AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                                             kMemoryByteLength),
            AMDF_STATUS_OK);
  ASSERT_EQ(api_->host_mapping_cache_control(target_mapping_,
                                             AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                                             kMemoryByteLength),
            AMDF_STATUS_OK);

  amdf_gpu_user_queue_create_info_t create_info = {};
  create_info.type = AMDF_STRUCTURE_TYPE_GPU_USER_QUEUE_CREATE_INFO;
  create_info.structure_size = sizeof(create_info);
  create_info.queue_family_ordinal = queue_family_ordinal;
  create_info.priority = AMDF_QUEUE_PRIORITY_NORMAL;
  create_info.producer_mode = AMDF_QUEUE_PRODUCER_MODE_SINGLE;
  create_info.required_capabilities = AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER;
  ASSERT_EQ(gpu_api_->user_queue_create(device_, &create_info, &queue_),
            AMDF_STATUS_OK);

  amdf_user_queue_info_t queue_info = {};
  queue_info.type = AMDF_STRUCTURE_TYPE_USER_QUEUE_INFO;
  queue_info.structure_size = sizeof(queue_info);
  ASSERT_EQ(api_->user_queue_query_info(queue_, &queue_info), AMDF_STATUS_OK);
  EXPECT_EQ(queue_info.queue_family_ordinal, queue_family_ordinal);
  EXPECT_EQ(queue_info.command_type, command_type);
  EXPECT_EQ(queue_info.format_version, QueueFormatVersion(command_type));
  EXPECT_EQ(queue_info.producer_mode, AMDF_QUEUE_PRODUCER_MODE_SINGLE);
  EXPECT_EQ(queue_info.priority, AMDF_QUEUE_PRIORITY_NORMAL);
  EXPECT_EQ(queue_info.capabilities, AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER);
  EXPECT_EQ(queue_info.roles & kRequiredQueueRoles, kRequiredQueueRoles);
  EXPECT_EQ(queue_info.metadata.command_type, AMDF_QUEUE_COMMAND_TYPE_UNKNOWN);
  EXPECT_EQ(queue_info.metadata_ring_byte_length, 0u);
  EXPECT_TRUE(amdf_device_id_is_equal(&queue_info.device_id,
                                      &source_memory_info_.device_id));
  EXPECT_TRUE(amdf_device_id_is_equal(&queue_info.device_id,
                                      &target_memory_info_.device_id));

  ASSERT_EQ(api_->user_queue_map(queue_, nullptr, &queue_mapping_),
            AMDF_STATUS_OK);
  amdf_user_queue_mapping_info_t mapping_info = {};
  mapping_info.type = AMDF_STRUCTURE_TYPE_USER_QUEUE_MAPPING_INFO;
  mapping_info.structure_size = sizeof(mapping_info);
  ASSERT_EQ(api_->user_queue_mapping_query_info(queue_mapping_, &mapping_info),
            AMDF_STATUS_OK);
  EXPECT_TRUE(
      amdf_queue_id_is_equal(&mapping_info.queue_id, &queue_info.queue_id));
  EXPECT_EQ(mapping_info.producer_device_id.words[0] |
                mapping_info.producer_device_id.words[1],
            0u);
  EXPECT_EQ(mapping_info.queue_reset_epoch, queue_info.reset_epoch);
  EXPECT_EQ(mapping_info.producer_reset_epoch, 0u);
  EXPECT_EQ(mapping_info.command_type, queue_info.command_type);
  EXPECT_EQ(mapping_info.format_version, queue_info.format_version);
  EXPECT_EQ(mapping_info.ring_byte_length, queue_info.ring_byte_length);
  EXPECT_EQ(mapping_info.index_bits, 64u);
  EXPECT_EQ(mapping_info.doorbell_bits, 64u);
  EXPECT_EQ(mapping_info.metadata.command_type,
            AMDF_QUEUE_COMMAND_TYPE_UNKNOWN);
  EXPECT_EQ(mapping_info.metadata_ring_address, 0u);
  EXPECT_EQ(mapping_info.metadata_ring_byte_length, 0u);
  ASSERT_NE(mapping_info.ring_address, 0u);
  ASSERT_NE(mapping_info.read_index_address, 0u);
  ASSERT_NE(mapping_info.write_index_address, 0u);
  ASSERT_NE(mapping_info.doorbell_address, 0u);
  ASSERT_EQ(mapping_info.ring_address & (sizeof(uint32_t) - 1), 0u);
  ASSERT_EQ(mapping_info.read_index_address & (sizeof(uint64_t) - 1), 0u);
  ASSERT_EQ(mapping_info.write_index_address & (sizeof(uint64_t) - 1), 0u);
  ASSERT_EQ(mapping_info.doorbell_address & (sizeof(uint64_t) - 1), 0u);
  ASSERT_GE(mapping_info.ring_byte_length,
            kPm4PublishedDwordCount * sizeof(uint32_t));

  auto* read_index = reinterpret_cast<volatile uint64_t*>(
      static_cast<uintptr_t>(mapping_info.read_index_address));
  auto* write_index = reinterpret_cast<volatile uint64_t*>(
      static_cast<uintptr_t>(mapping_info.write_index_address));
  auto* doorbell = reinterpret_cast<volatile uint64_t*>(
      static_cast<uintptr_t>(mapping_info.doorbell_address));
  EXPECT_EQ(__atomic_load_n(read_index, __ATOMIC_ACQUIRE), 0u);
  EXPECT_EQ(__atomic_load_n(write_index, __ATOMIC_ACQUIRE), 0u);

  amdf_user_queue_status_t queue_status = {};
  queue_status.type = AMDF_STRUCTURE_TYPE_USER_QUEUE_STATUS;
  queue_status.structure_size = sizeof(queue_status);
  ASSERT_EQ(api_->user_queue_query_status(queue_, &queue_status),
            AMDF_STATUS_OK);
  EXPECT_EQ(queue_status.state, AMDF_QUEUE_STATE_ACTIVE);
  EXPECT_EQ(queue_status.reset_epoch, queue_info.reset_epoch);
  EXPECT_EQ(queue_status.producer_index, 0u);
  EXPECT_EQ(queue_status.consumed_index, 0u);
  EXPECT_EQ(queue_status.terminal_status, AMDF_STATUS_OK);

  auto* ring = reinterpret_cast<uint32_t*>(
      static_cast<uintptr_t>(mapping_info.ring_address));
  const EncodedQueueStream stream =
      EncodeCopyStream(command_type, ring, source_memory_info_.device_address,
                       target_memory_info_.device_address);
  if (command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_PM4) {
    ASSERT_EQ(stream.byte_length, kPm4PublishedDwordCount * sizeof(uint32_t));
    ASSERT_EQ(stream.published_index, kPm4PublishedDwordCount);
  } else {
    ASSERT_EQ(stream.byte_length, kSdmaPublishedDwordCount * sizeof(uint32_t));
    ASSERT_EQ(stream.published_index, stream.byte_length);
  }
  ASSERT_LE(stream.byte_length, mapping_info.ring_byte_length);
  __atomic_store_n(write_index, stream.published_index, __ATOMIC_RELEASE);
  __atomic_store_n(doorbell, stream.published_index, __ATOMIC_RELEASE);

  ASSERT_EQ(
      api_->user_queue_wait_consumed(queue_, stream.published_index,
                                     AMDF_TIMEOUT_INFINITE, UINT64_C(10000000)),
      AMDF_STATUS_OK);
  ASSERT_EQ(api_->user_queue_query_status(queue_, &queue_status),
            AMDF_STATUS_OK);
  EXPECT_EQ(queue_status.state, AMDF_QUEUE_STATE_ACTIVE);
  EXPECT_EQ(queue_status.reset_epoch, queue_info.reset_epoch);
  EXPECT_EQ(queue_status.producer_index, stream.published_index);
  EXPECT_EQ(queue_status.consumed_index, stream.published_index);
  EXPECT_EQ(queue_status.terminal_status, AMDF_STATUS_OK);

  ASSERT_EQ(api_->host_mapping_cache_control(
                target_mapping_, AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0,
                kMemoryByteLength),
            AMDF_STATUS_OK);
  ASSERT_EQ(api_->host_mapping_cache_control(
                source_mapping_, AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0,
                kMemoryByteLength),
            AMDF_STATUS_OK);
  for (size_t i = 0; i < kElementCount; ++i) {
    EXPECT_EQ(target[i], expected[i]) << "target word " << i;
    EXPECT_EQ(source[i], expected[i]) << "source word " << i;
  }
  EXPECT_EQ(*completion, kCompletionValue);
}

}  // namespace

bool RunGfx1151UserQueueMemoryCopies(const amdf_api_t* api,
                                     const amdf_gpu_api_t* gpu_api,
                                     amdf_endpoint_t* endpoint,
                                     amdf_device_t* device,
                                     amdf_queue_command_type_t command_type) {
  UserQueueMemoryScenario scenario(api, gpu_api, endpoint, device);
  scenario.RunCopiesBetweenExactAccessAttachments(command_type);
  return scenario.Release();
}

namespace {

class Gfx1151UserQueueMemoryTest : public GpuDeviceFixture {
 protected:
  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) const override {
    amdf_gpu_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    const amdf_status_t status = gpu_api_->endpoint_query_info(endpoint, &info);
    if (amdf_status_is_ok(status)) {
      *out_matches = info.gfx_ip.major == 11 && info.gfx_ip.minor == 5 &&
                     info.gfx_ip.stepping == 1;
    }
    return status;
  }

  void RunCopiesBetweenExactAccessAttachments(
      amdf_queue_command_type_t command_type) {
    const bool children_released = RunGfx1151UserQueueMemoryCopies(
        api_, gpu_api_, endpoint_, device_, command_type);
    ASSERT_TRUE(children_released);
  }
};

TEST_F(Gfx1151UserQueueMemoryTest, Pm4CopiesBetweenExactAccessAttachments) {
  RunCopiesBetweenExactAccessAttachments(AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
}

TEST_F(Gfx1151UserQueueMemoryTest, SdmaCopiesBetweenExactAccessAttachments) {
  RunCopiesBetweenExactAccessAttachments(AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
}

}  // namespace
