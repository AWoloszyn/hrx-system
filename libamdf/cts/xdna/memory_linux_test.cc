// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#include "amdf/amdf.h"
#include "gtest/gtest.h"
#include "xdna_device_fixture.h"

namespace {

class XdnaLinuxMemoryTest : public XdnaDeviceFixture {
 protected:
  void TearDown() override {
    bool caller_pages_may_release = true;
    for (size_t i = 0; i < 2; ++i) {
      if (mappings_[i] != nullptr) {
        const amdf_status_t status = api_->host_mapping_destroy(mappings_[i]);
        EXPECT_EQ(status, AMDF_STATUS_OK);
        if (amdf_status_is_ok(status)) {
          mappings_[i] = nullptr;
        } else {
          caller_pages_may_release = false;
        }
      }
      if (memories_[i] != nullptr && mappings_[i] == nullptr) {
        const amdf_status_t status = api_->memory_destroy(memories_[i]);
        EXPECT_EQ(status, AMDF_STATUS_OK);
        if (amdf_status_is_ok(status)) {
          memories_[i] = nullptr;
        } else {
          caller_pages_may_release = false;
        }
      }
    }
    if (caller_pages_ != nullptr && caller_pages_may_release) {
      EXPECT_EQ(munmap(caller_pages_, caller_byte_length_), 0);
      caller_pages_ = nullptr;
    }
    XdnaDeviceFixture::TearDown();
  }

  void AllocateCallerPages() {
    const long page_size = sysconf(_SC_PAGESIZE);
    ASSERT_GT(page_size, 0);
    page_size_ = static_cast<size_t>(page_size);
    caller_byte_length_ = page_size_ * 8;
    void* pages = mmap(nullptr, caller_byte_length_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(pages, MAP_FAILED);
    caller_pages_ = static_cast<uint8_t*>(pages);
    std::memset(caller_pages_, 0x5A, caller_byte_length_);
  }

  amdf_status_t MapMemory(amdf_memory_t* memory, uint32_t ordinal,
                          uint64_t byte_length) {
    const amdf_memory_map_info_t map_info = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO,
        .structure_size = sizeof(amdf_memory_map_info_t),
        .byte_length = byte_length,
        .flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
    };
    amdf_status_t status =
        api_->memory_map(memory, &map_info, &mappings_[ordinal]);
    if (!amdf_status_is_ok(status)) return status;
    mapping_infos_[ordinal] = {};
    mapping_infos_[ordinal].type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    mapping_infos_[ordinal].structure_size = sizeof(mapping_infos_[ordinal]);
    return api_->host_mapping_query_info(mappings_[ordinal],
                                         &mapping_infos_[ordinal]);
  }

  // Host page size governing the native registration cover.
  size_t page_size_ = 0;
  // Complete caller-owned mapping retained through every registration.
  size_t caller_byte_length_ = 0;
  // First byte of the caller-owned mapping.
  uint8_t* caller_pages_ = nullptr;
  // Independent overlapping XDNA attachments.
  amdf_memory_t* memories_[2] = {};
  // Independent host views of the overlapping attachments.
  amdf_host_mapping_t* mappings_[2] = {};
  // Immutable facts for each host view.
  amdf_host_mapping_info_t mapping_infos_[2] = {};
};

TEST_F(XdnaLinuxMemoryTest,
       RegistersArbitraryOverlappingCallerSubrangesWithoutTakingOwnership) {
  ASSERT_NO_FATAL_FAILURE(AllocateCallerPages());
  const amdf_memory_flags_t required_flags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  const amdf_memory_access_t device_access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  const uint32_t profile_ordinal = FindMemoryProfileOrdinal(
      AMDF_MEMORY_CLASS_REGISTERED_HOST,
      AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
      required_flags, device_access);
  ASSERT_NE(profile_ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);

  amdf_memory_create_info_t create_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO,
      .structure_size = sizeof(amdf_memory_create_info_t),
      .memory_profile_ordinal = profile_ordinal,
      .device_access = device_access,
      .required_flags = required_flags,
      .byte_length = caller_byte_length_ / 2 + 17,
      .minimum_alignment = 1,
      .registered_host_pointer = caller_pages_ + 3,
  };

  amdf_memory_t* output = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_create_info_t unsupported_info = create_info;
  unsupported_info.device_access = AMDF_MEMORY_ACCESS_READ;
  EXPECT_EQ(amdf_status_code(
                api_->memory_create(device_, &unsupported_info, &output)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(output), uintptr_t{1});
  EXPECT_EQ(caller_pages_[3], 0x5A);

  ASSERT_EQ(api_->memory_create(device_, &create_info, &memories_[0]),
            AMDF_STATUS_OK);
  amdf_memory_info_t first_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_INFO,
      .structure_size = sizeof(amdf_memory_info_t),
  };
  ASSERT_EQ(api_->memory_query_info(memories_[0], &first_info), AMDF_STATUS_OK);
  EXPECT_EQ(first_info.memory_class, AMDF_MEMORY_CLASS_REGISTERED_HOST);
  EXPECT_EQ(first_info.device_access, device_access);
  EXPECT_EQ(first_info.flags & required_flags, required_flags);
  EXPECT_EQ(first_info.source_byte_offset, 3u);
  EXPECT_EQ(first_info.byte_length, create_info.byte_length);
  EXPECT_EQ(first_info.alignment, 1u);
  EXPECT_EQ(first_info.native_allocation_byte_length,
            ((first_info.source_byte_offset + first_info.byte_length +
              page_size_ - 1) /
             page_size_) *
                page_size_);
  EXPECT_EQ(first_info.native_allocation_granularity, page_size_);
  EXPECT_FALSE(
      amdf_physical_memory_id_is_valid(&first_info.physical_backing_id));
  EXPECT_EQ(first_info.device_address & (page_size_ - 1),
            first_info.source_byte_offset);
  ASSERT_EQ(MapMemory(memories_[0], 0, create_info.byte_length),
            AMDF_STATUS_OK);
  EXPECT_EQ(mapping_infos_[0].pointer, create_info.registered_host_pointer);
  EXPECT_EQ(mapping_infos_[0].cacheability, AMDF_HOST_CACHEABILITY_WRITE_BACK);

  create_info.registered_host_pointer = caller_pages_ + 19;
  create_info.byte_length = caller_byte_length_ / 2;
  ASSERT_EQ(api_->memory_create(device_, &create_info, &memories_[1]),
            AMDF_STATUS_OK);
  amdf_memory_info_t second_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_INFO,
      .structure_size = sizeof(amdf_memory_info_t),
  };
  ASSERT_EQ(api_->memory_query_info(memories_[1], &second_info),
            AMDF_STATUS_OK);
  EXPECT_EQ(second_info.source_byte_offset, 19u);
  EXPECT_EQ(second_info.byte_length, create_info.byte_length);
  EXPECT_EQ(second_info.device_address & (page_size_ - 1),
            second_info.source_byte_offset);
  ASSERT_EQ(MapMemory(memories_[1], 1, create_info.byte_length),
            AMDF_STATUS_OK);
  EXPECT_EQ(mapping_infos_[1].pointer, create_info.registered_host_pointer);

  std::memset(mapping_infos_[1].pointer, 0xC3, create_info.byte_length);
  ASSERT_EQ(api_->host_mapping_cache_control(mappings_[1],
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

  ASSERT_EQ(api_->host_mapping_destroy(mappings_[0]), AMDF_STATUS_OK);
  mappings_[0] = nullptr;
  ASSERT_EQ(api_->memory_destroy(memories_[0]), AMDF_STATUS_OK);
  memories_[0] = nullptr;
  EXPECT_EQ(static_cast<const uint8_t*>(mapping_infos_[1].pointer)[0], 0xC3);
  EXPECT_EQ(static_cast<const uint8_t*>(
                mapping_infos_[1].pointer)[create_info.byte_length - 1],
            0xC3);

  ASSERT_EQ(api_->host_mapping_destroy(mappings_[1]), AMDF_STATUS_OK);
  mappings_[1] = nullptr;
  ASSERT_EQ(api_->memory_destroy(memories_[1]), AMDF_STATUS_OK);
  memories_[1] = nullptr;
  std::memset(caller_pages_, 0x3C, caller_byte_length_);
  EXPECT_EQ(caller_pages_[caller_byte_length_ - 1], 0x3C);
}

}  // namespace
