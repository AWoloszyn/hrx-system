// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "amdf/amdf.h"
#include "amdf/gpu.h"
#include "gpu_device_fixture.h"
#include "gtest/gtest.h"

namespace {

class GpuLinuxMemoryTest : public GpuDeviceFixture {
 protected:
  void TearDown() override {
    for (amdf_host_mapping_t*& mapping : mappings_) {
      if (mapping == nullptr) continue;
      ASSERT_EQ(api_->host_mapping_destroy(mapping), AMDF_STATUS_OK);
      mapping = nullptr;
    }
    for (amdf_memory_t*& memory : memories_) {
      if (memory == nullptr) continue;
      ASSERT_EQ(api_->memory_destroy(memory), AMDF_STATUS_OK);
      memory = nullptr;
    }
    if (caller_pages_ != nullptr) {
      ASSERT_EQ(munmap(caller_pages_, caller_byte_length_), 0);
      caller_pages_ = nullptr;
    }
    GpuDeviceFixture::TearDown();
  }

  amdf_memory_create_info_t MakeSystemMemoryCreateInfo() {
    amdf_memory_create_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    info.structure_size = sizeof(info);
    info.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    info.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                          AMDF_MEMORY_FLAG_HOST_COHERENT |
                          AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    info.byte_length = 4097;
    info.minimum_alignment = 1024 * 1024;
    return info;
  }

  amdf_status_t MapMemory(amdf_memory_t* memory, uint32_t ordinal,
                          uint64_t byte_offset, uint64_t byte_length,
                          amdf_memory_map_flags_t flags) {
    amdf_memory_map_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
    info.structure_size = sizeof(info);
    info.byte_offset = byte_offset;
    info.byte_length = byte_length;
    info.flags = flags;
    amdf_status_t status = api_->memory_map(memory, &info, &mappings_[ordinal]);
    if (!amdf_status_is_ok(status)) return status;
    mapping_infos_[ordinal] = {};
    mapping_infos_[ordinal].type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    mapping_infos_[ordinal].structure_size = sizeof(mapping_infos_[ordinal]);
    return api_->host_mapping_query_info(mappings_[ordinal],
                                         &mapping_infos_[ordinal]);
  }

  void AllocateCallerPages() {
    const long page_size = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page_size, 0);
    caller_byte_length_ = static_cast<size_t>(page_size) * 8;
    void* pages = mmap(nullptr, caller_byte_length_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(pages, MAP_FAILED);
    caller_pages_ = static_cast<uint8_t*>(pages);
    std::memset(caller_pages_, 0x5A, caller_byte_length_);
  }

  // Exercises the same allocation/host-view lifecycle in either context mode.
  void ExerciseSystemMemory() {
    const amdf_memory_create_info_t create_info = MakeSystemMemoryCreateInfo();
    ASSERT_EQ(api_->memory_create(device_, &create_info, &memories_[0]),
              AMDF_STATUS_OK);
    amdf_memory_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
    info.structure_size = sizeof(info);
    ASSERT_EQ(api_->memory_query_info(memories_[0], &info), AMDF_STATUS_OK);
    EXPECT_EQ(info.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
    EXPECT_EQ(info.flags & create_info.required_flags,
              create_info.required_flags);
    EXPECT_GE(info.byte_length, create_info.byte_length);
    ASSERT_GE(info.alignment, create_info.minimum_alignment);
    EXPECT_NE(info.device_address, 0u);
    EXPECT_EQ(info.device_address & (info.alignment - 1), 0u);
    EXPECT_NE(
        info.physical_backing_id.words[0] | info.physical_backing_id.words[1],
        0u);

    ASSERT_EQ(MapMemory(memories_[0], 0, 0, info.byte_length,
                        AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE),
              AMDF_STATUS_OK);
    ASSERT_NE(mapping_infos_[0].pointer, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(mapping_infos_[0].pointer) &
                  (info.alignment - 1),
              0u);
    EXPECT_EQ(mapping_infos_[0].cacheability, AMDF_HOST_CACHEABILITY_COHERENT);
    EXPECT_EQ(mapping_infos_[0].reset_epoch, info.reset_epoch);
    std::memset(mapping_infos_[0].pointer, 0xA5,
                static_cast<size_t>(info.byte_length));

    ASSERT_EQ(MapMemory(memories_[0], 1, 128, 256,
                        AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE),
              AMDF_STATUS_OK);
    EXPECT_EQ(mapping_infos_[1].pointer,
              static_cast<uint8_t*>(mapping_infos_[0].pointer) + 128);
    const auto* bytes = static_cast<const uint8_t*>(mapping_infos_[1].pointer);
    for (size_t i = 0; i < 256; ++i) EXPECT_EQ(bytes[i], 0xA5);
    EXPECT_EQ(api_->host_mapping_cache_control(
                  mappings_[1], AMDF_HOST_CACHE_OPERATION_FLUSH, 0, 256),
              AMDF_STATUS_OK);
    EXPECT_EQ(api_->host_mapping_cache_control(
                  mappings_[1], AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0, 256),
              AMDF_STATUS_OK);
    EXPECT_EQ(amdf_status_code(api_->host_mapping_cache_control(
                  mappings_[1], AMDF_HOST_CACHE_OPERATION_FLUSH, 255, 2)),
              AMDF_STATUS_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(amdf_status_code(api_->memory_destroy(memories_[0])),
              AMDF_STATUS_CODE_BUSY);
    EXPECT_EQ(amdf_status_code(api_->device_destroy(device_)),
              AMDF_STATUS_CODE_BUSY);

    ASSERT_EQ(api_->host_mapping_destroy(mappings_[0]), AMDF_STATUS_OK);
    mappings_[0] = nullptr;
    amdf_memory_info_t after = info;
    ASSERT_EQ(api_->memory_query_info(memories_[0], &after), AMDF_STATUS_OK);
    EXPECT_EQ(after.device_address, info.device_address);
    ASSERT_EQ(MapMemory(memories_[0], 0, 0, info.byte_length,
                        AMDF_MEMORY_MAP_FLAG_READ),
              AMDF_STATUS_OK);
    EXPECT_EQ(static_cast<const uint8_t*>(mapping_infos_[0].pointer)[128],
              0xA5);
    EXPECT_EQ(amdf_status_code(api_->host_mapping_cache_control(
                  mappings_[0], AMDF_HOST_CACHE_OPERATION_FLUSH, 0, 1)),
              AMDF_STATUS_CODE_FAILED_PRECONDITION);

    for (amdf_host_mapping_t*& mapping : mappings_) {
      ASSERT_EQ(api_->host_mapping_destroy(mapping), AMDF_STATUS_OK);
      mapping = nullptr;
    }
    ASSERT_EQ(api_->memory_destroy(memories_[0]), AMDF_STATUS_OK);
    memories_[0] = nullptr;
  }

  // Owned attachments released after every host view.
  std::array<amdf_memory_t*, 2> memories_ = {};
  // Explicit views borrowing the attachments above.
  std::array<amdf_host_mapping_t*, 2> mappings_ = {};
  // Cached properties of each explicit view.
  std::array<amdf_host_mapping_info_t, 2> mapping_infos_ = {};
  // Caller-owned anonymous pages retained until every registration is gone.
  uint8_t* caller_pages_ = nullptr;
  // Length of the caller-owned mmap reservation in bytes.
  size_t caller_byte_length_ = 0;
};

TEST_F(GpuLinuxMemoryTest, OwnsAlignedSystemMemoryAndBorrowedHostViews) {
  ASSERT_NO_FATAL_FAILURE(ExerciseSystemMemory());
}

TEST_F(GpuLinuxMemoryTest, RejectsUnachievableSystemPlacement) {
  for (amdf_memory_flags_t flag :
       {AMDF_MEMORY_FLAG_DEVICE_LOCAL, AMDF_MEMORY_FLAG_SHAREABLE}) {
    amdf_memory_create_info_t info = MakeSystemMemoryCreateInfo();
    info.required_flags |= flag;
    amdf_memory_t* output = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
    EXPECT_EQ(api_->memory_create(device_, &info, &output),
              amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
    EXPECT_EQ(output, nullptr);
  }
}

TEST_F(GpuLinuxMemoryTest, HonorsLocalPlacementCapabilities) {
  amdf_memory_create_info_t create_info = MakeSystemMemoryCreateInfo();
  create_info.memory_class = AMDF_MEMORY_CLASS_LOCAL;
  create_info.required_flags = AMDF_MEMORY_FLAG_DEVICE_LOCAL |
                               AMDF_MEMORY_FLAG_DEVICE_ADDRESS |
                               AMDF_MEMORY_FLAG_EXECUTABLE;
  const amdf_status_t status =
      api_->memory_create(device_, &create_info, &memories_[0]);
  if ((features_ & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) == 0) {
    EXPECT_EQ(status, amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
    EXPECT_EQ(memories_[0], nullptr);
    return;
  }
  ASSERT_EQ(status, AMDF_STATUS_OK);
  amdf_memory_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  info.structure_size = sizeof(info);
  ASSERT_EQ(api_->memory_query_info(memories_[0], &info), AMDF_STATUS_OK);
  EXPECT_EQ(info.memory_class, AMDF_MEMORY_CLASS_LOCAL);
  EXPECT_EQ(info.flags & create_info.required_flags,
            create_info.required_flags);
  EXPECT_EQ(MapMemory(memories_[0], 0, 0, 1, AMDF_MEMORY_MAP_FLAG_READ),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(mappings_[0], nullptr);

  create_info.required_flags |= AMDF_MEMORY_FLAG_HOST_VISIBLE;
  const amdf_status_t visible_status =
      api_->memory_create(device_, &create_info, &memories_[1]);
  if ((features_ & AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY) == 0) {
    EXPECT_EQ(visible_status,
              amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
    EXPECT_EQ(memories_[1], nullptr);
    return;
  }
  ASSERT_EQ(visible_status, AMDF_STATUS_OK);
  ASSERT_EQ(MapMemory(memories_[1], 0, 0, create_info.byte_length,
                      AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE),
            AMDF_STATUS_OK);
  std::memset(mapping_infos_[0].pointer, 0xA5, create_info.byte_length);
  EXPECT_EQ(api_->host_mapping_cache_control(mappings_[0],
                                             AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                                             create_info.byte_length),
            AMDF_STATUS_OK);
}

TEST_F(GpuLinuxMemoryTest, RejectsRegistrationWhenModeDoesNotSupportIt) {
  if ((features_ & AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION) != 0) {
    GTEST_SKIP() << "selected mode supports host registration";
  }
  ASSERT_NO_FATAL_FAILURE(AllocateCallerPages());
  amdf_memory_create_info_t info = MakeSystemMemoryCreateInfo();
  info.memory_class = AMDF_MEMORY_CLASS_REGISTERED_HOST;
  info.byte_length = caller_byte_length_;
  info.minimum_alignment = 0;
  info.registered_host_pointer = caller_pages_;
  amdf_memory_t* output = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  EXPECT_EQ(api_->memory_create(device_, &info, &output),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(output, nullptr);
}

TEST_F(GpuLinuxMemoryTest, RejectsUnadvertisedKernelQueueCreation) {
  amdf_endpoint_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
  info.structure_size = sizeof(info);
  ASSERT_EQ(api_->endpoint_query_info(endpoint_, &info), AMDF_STATUS_OK);
  for (uint32_t ordinal = 0; ordinal < info.queue_family_count; ++ordinal) {
    amdf_queue_family_info_t family = {};
    family.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
    family.structure_size = sizeof(family);
    ASSERT_EQ(
        api_->endpoint_query_queue_family_info(endpoint_, ordinal, &family),
        AMDF_STATUS_OK);
    EXPECT_EQ(family.publication_modes & AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
              0u);
  }
  amdf_gpu_kernel_queue_create_info_t create_info = {};
  create_info.type = AMDF_STRUCTURE_TYPE_GPU_KERNEL_QUEUE_CREATE_INFO;
  create_info.structure_size = sizeof(create_info);
  create_info.queue_family_ordinal = info.queue_family_count;
  amdf_kernel_queue_t* output =
      reinterpret_cast<amdf_kernel_queue_t*>(uintptr_t{1});
  EXPECT_EQ(amdf_status_code(
                gpu_api_->kernel_queue_create(device_, &create_info, &output)),
            AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(output, nullptr);
}

// Process mode has one long-lived device in this test process. Its native VM
// binding survives destruction, so each registration case uses that same owner.
class GpuLinuxProcessMemoryTest : public GpuLinuxMemoryTest {
 protected:
  amdf_gpu_device_mode_t GetDeviceMode() const override {
    return AMDF_GPU_DEVICE_MODE_PROCESS;
  }
};

TEST_F(GpuLinuxProcessMemoryTest, OwnsMemoryAndOverlappingCallerRegistrations) {
  ASSERT_NE(features_ & AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION, 0u);
  EXPECT_EQ(features_ & AMDF_GPU_DEVICE_FEATURE_DEVICE_RECREATION, 0u);
  amdf_gpu_device_info_t device_info = {};
  device_info.type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_INFO;
  device_info.structure_size = sizeof(device_info);
  ASSERT_EQ(gpu_api_->device_query_info(device_, &device_info), AMDF_STATUS_OK);
  EXPECT_EQ(device_info.mode, AMDF_GPU_DEVICE_MODE_PROCESS);
  EXPECT_EQ(device_info.features, features_);
  ASSERT_NO_FATAL_FAILURE(ExerciseSystemMemory());
  ASSERT_NO_FATAL_FAILURE(AllocateCallerPages());

  amdf_memory_create_info_t create_info = MakeSystemMemoryCreateInfo();
  create_info.memory_class = AMDF_MEMORY_CLASS_REGISTERED_HOST;
  create_info.byte_length = caller_byte_length_ / 2 + 17;
  create_info.minimum_alignment = 1;
  create_info.registered_host_pointer = caller_pages_ + 3;
  ASSERT_EQ(api_->memory_create(device_, &create_info, &memories_[0]),
            AMDF_STATUS_OK);
  amdf_memory_info_t first_info = {};
  first_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  first_info.structure_size = sizeof(first_info);
  ASSERT_EQ(api_->memory_query_info(memories_[0], &first_info), AMDF_STATUS_OK);
  EXPECT_EQ(first_info.memory_class, AMDF_MEMORY_CLASS_REGISTERED_HOST);
  EXPECT_EQ(first_info.flags & create_info.required_flags,
            create_info.required_flags);
  EXPECT_EQ(first_info.byte_length, create_info.byte_length);
  EXPECT_EQ(first_info.alignment, 1u);
  ASSERT_EQ(MapMemory(memories_[0], 0, 0, create_info.byte_length,
                      AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE),
            AMDF_STATUS_OK);
  EXPECT_EQ(mapping_infos_[0].pointer, caller_pages_ + 3);
  EXPECT_EQ(mapping_infos_[0].cacheability, AMDF_HOST_CACHEABILITY_COHERENT);

  create_info.registered_host_pointer = caller_pages_ + 19;
  create_info.byte_length = caller_byte_length_ / 2;
  ASSERT_EQ(api_->memory_create(device_, &create_info, &memories_[1]),
            AMDF_STATUS_OK);
  amdf_memory_info_t second_info = first_info;
  ASSERT_EQ(api_->memory_query_info(memories_[1], &second_info),
            AMDF_STATUS_OK);
  EXPECT_NE(second_info.device_address, first_info.device_address + 16);
  ASSERT_EQ(MapMemory(memories_[1], 1, 0, create_info.byte_length,
                      AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE),
            AMDF_STATUS_OK);
  EXPECT_EQ(mapping_infos_[1].pointer, caller_pages_ + 19);
  std::memset(mapping_infos_[1].pointer, 0xC3, create_info.byte_length);
  EXPECT_EQ(api_->host_mapping_cache_control(mappings_[1],
                                             AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                                             create_info.byte_length),
            AMDF_STATUS_OK);
  const auto* first_bytes =
      static_cast<const uint8_t*>(mapping_infos_[0].pointer);
  for (size_t i = 0; i < create_info.byte_length; ++i) {
    ASSERT_EQ(first_bytes[16 + i], 0xC3) << "byte " << i;
  }
  EXPECT_EQ(caller_pages_[2], 0x5A);
  EXPECT_EQ(caller_pages_[19 + create_info.byte_length], 0x5A);

  amdf_memory_map_info_t overrun = {};
  overrun.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
  overrun.structure_size = sizeof(overrun);
  overrun.flags = AMDF_MEMORY_MAP_FLAG_READ;
  overrun.byte_length = first_info.byte_length + 1;
  amdf_host_mapping_t* output =
      reinterpret_cast<amdf_host_mapping_t*>(uintptr_t{1});
  EXPECT_EQ(amdf_status_code(api_->memory_map(memories_[0], &overrun, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(output, nullptr);

  for (amdf_host_mapping_t*& mapping : mappings_) {
    ASSERT_EQ(api_->host_mapping_destroy(mapping), AMDF_STATUS_OK);
    mapping = nullptr;
  }
  for (amdf_memory_t*& memory : memories_) {
    ASSERT_EQ(api_->memory_destroy(memory), AMDF_STATUS_OK);
    memory = nullptr;
  }
  std::memset(caller_pages_, 0x3C, caller_byte_length_);
  EXPECT_EQ(caller_pages_[caller_byte_length_ - 1], 0x3C);
}

}  // namespace
