// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "amdf/amdf.h"
#include "amdf/gpu.h"
#include "amdf/xdna.h"
#include "gtest/gtest.h"
#include "util/provider.h"

namespace {

const amdf_external_memory_support_t* FindDmaBufSupport(
    const amdf_memory_profile_t& profile,
    amdf_external_memory_support_flags_t required_flags) {
  for (uint32_t i = 0; i < profile.external_memory_support_count; ++i) {
    const amdf_external_memory_support_t& support =
        profile.external_memory_support[i];
    if (support.type == AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD &&
        (support.flags & required_flags) == required_flags) {
      return &support;
    }
  }
  return nullptr;
}

amdf_status_t FindDmaBufProfile(
    const amdf_api_t* api, amdf_device_t* device,
    amdf_memory_profile_roles_t required_roles,
    amdf_memory_flags_t required_memory_flags,
    amdf_external_memory_support_flags_t required_external_flags,
    uint32_t* out_ordinal, amdf_memory_profile_t* out_profile) {
  for (uint32_t ordinal = 0; ordinal != UINT32_MAX; ++ordinal) {
    amdf_memory_profile_t profile = {};
    profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
    profile.structure_size = sizeof(profile);
    const amdf_status_t status =
        api->device_query_memory_profile(device, ordinal, &profile);
    if (amdf_status_code(status) == AMDF_STATUS_CODE_OUT_OF_RANGE) {
      *out_ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
      return AMDF_STATUS_OK;
    }
    if (!amdf_status_is_ok(status)) return status;
    if (profile.memory_class == AMDF_MEMORY_CLASS_SYSTEM &&
        (profile.roles & required_roles) == required_roles &&
        (required_memory_flags & ~profile.supported_flags) == 0 &&
        FindDmaBufSupport(profile, required_external_flags) != nullptr) {
      *out_ordinal = ordinal;
      *out_profile = profile;
      return AMDF_STATUS_OK;
    }
  }
  return amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
}

class GpuXdnaMemoryInteropTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(amdf_cts_provider_query_api()(AMDF_ABI_VERSION_1,
                                            AMDF_ABI_VERSION_LATEST, &api_),
              AMDF_STATUS_OK);
    ASSERT_NE(api_, nullptr);

    const void* extension_api = nullptr;
    ASSERT_EQ(api_->query_extension(
                  AMDF_EXTENSION_GPU, AMDF_GPU_EXTENSION_VERSION_1,
                  AMDF_GPU_EXTENSION_VERSION_LATEST, &extension_api),
              AMDF_STATUS_OK);
    gpu_api_ = static_cast<const amdf_gpu_api_t*>(extension_api);
    ASSERT_NE(gpu_api_, nullptr);
    extension_api = nullptr;
    ASSERT_EQ(api_->query_extension(
                  AMDF_EXTENSION_XDNA, AMDF_XDNA_EXTENSION_VERSION_1,
                  AMDF_XDNA_EXTENSION_VERSION_LATEST, &extension_api),
              AMDF_STATUS_OK);
    xdna_api_ = static_cast<const amdf_xdna_api_t*>(extension_api);
    ASSERT_NE(xdna_api_, nullptr);

    amdf_instance_create_info_t instance_create_info = {};
    instance_create_info.type = AMDF_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.structure_size = sizeof(instance_create_info);
    amdf_status_t status =
        api_->instance_create(&instance_create_info, &instance_);
    if (amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "platform provider is unavailable";
    }
    ASSERT_EQ(status, AMDF_STATUS_OK);

    uint32_t endpoint_count = 0;
    ASSERT_EQ(api_->endpoint_enumerate(instance_, 0, nullptr, &endpoint_count),
              AMDF_STATUS_OK);
    std::vector<amdf_endpoint_summary_t> summaries(endpoint_count);
    if (endpoint_count != 0) {
      ASSERT_EQ(api_->endpoint_enumerate(instance_, endpoint_count,
                                         summaries.data(), &endpoint_count),
                AMDF_STATUS_OK);
    }
    for (const amdf_endpoint_summary_t& summary : summaries) {
      amdf_endpoint_t** endpoint = nullptr;
      if (summary.engine_kind == AMDF_ENGINE_KIND_GPU &&
          gpu_endpoint_ == nullptr) {
        endpoint = &gpu_endpoint_;
      } else if (summary.engine_kind == AMDF_ENGINE_KIND_XDNA &&
                 xdna_endpoint_ == nullptr) {
        endpoint = &xdna_endpoint_;
      }
      if (endpoint != nullptr) {
        ASSERT_EQ(api_->endpoint_open(instance_, &summary.id, endpoint),
                  AMDF_STATUS_OK);
      }
    }
    if (gpu_endpoint_ == nullptr || xdna_endpoint_ == nullptr) {
      GTEST_SKIP() << "a qualified GPU and XDNA endpoint pair is required";
    }

    amdf_gpu_device_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    status = gpu_api_->endpoint_query_device_capabilities(
        gpu_endpoint_, AMDF_GPU_DEVICE_MODE_INDEPENDENT, &capabilities);
    if (amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "independent KFD device mode is unavailable";
    }
    ASSERT_EQ(status, AMDF_STATUS_OK);

    amdf_gpu_device_create_info_t gpu_create_info = {};
    gpu_create_info.type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_CREATE_INFO;
    gpu_create_info.structure_size = sizeof(gpu_create_info);
    gpu_create_info.mode = AMDF_GPU_DEVICE_MODE_INDEPENDENT;
    ASSERT_EQ(
        gpu_api_->device_create(gpu_endpoint_, &gpu_create_info, &gpu_device_),
        AMDF_STATUS_OK);

    amdf_xdna_device_create_info_t xdna_create_info = {};
    xdna_create_info.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_CREATE_INFO;
    xdna_create_info.structure_size = sizeof(xdna_create_info);
    status = xdna_api_->device_create(xdna_endpoint_, &xdna_create_info,
                                      &xdna_device_);
    if (amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "XDNA ordinary-address-domain creation is unavailable";
    }
    ASSERT_EQ(status, AMDF_STATUS_OK);
  }

  void TearDown() override {
    if (api_ != nullptr) {
      api_->external_memory_release(&external_memory_);
    }
    if (xdna_mapping_ != nullptr) {
      const amdf_status_t status = api_->host_mapping_destroy(xdna_mapping_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) xdna_mapping_ = nullptr;
    }
    if (gpu_mapping_ != nullptr) {
      const amdf_status_t status = api_->host_mapping_destroy(gpu_mapping_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) gpu_mapping_ = nullptr;
    }
    if (xdna_mapping_ == nullptr && xdna_memory_ != nullptr) {
      const amdf_status_t status = api_->memory_destroy(xdna_memory_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) xdna_memory_ = nullptr;
    }
    if (gpu_mapping_ == nullptr && gpu_memory_ != nullptr) {
      const amdf_status_t status = api_->memory_destroy(gpu_memory_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) gpu_memory_ = nullptr;
    }
    if (xdna_memory_ == nullptr && xdna_device_ != nullptr) {
      const amdf_status_t status = api_->device_destroy(xdna_device_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) xdna_device_ = nullptr;
    }
    if (gpu_memory_ == nullptr && gpu_device_ != nullptr) {
      const amdf_status_t status = api_->device_destroy(gpu_device_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) gpu_device_ = nullptr;
    }
    if (xdna_device_ == nullptr && xdna_endpoint_ != nullptr) {
      const amdf_status_t status = api_->endpoint_close(xdna_endpoint_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) xdna_endpoint_ = nullptr;
    }
    if (gpu_device_ == nullptr && gpu_endpoint_ != nullptr) {
      const amdf_status_t status = api_->endpoint_close(gpu_endpoint_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) gpu_endpoint_ = nullptr;
    }
    if (gpu_endpoint_ == nullptr && xdna_endpoint_ == nullptr &&
        instance_ != nullptr) {
      const amdf_status_t status = api_->instance_destroy(instance_);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(status)) instance_ = nullptr;
    }
  }

  amdf_status_t Map(amdf_memory_t* memory, uint64_t byte_length,
                    amdf_host_mapping_t** out_mapping,
                    amdf_host_mapping_info_t* out_info) {
    amdf_memory_map_info_t map_info = {};
    map_info.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
    map_info.structure_size = sizeof(map_info);
    map_info.byte_length = byte_length;
    map_info.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
    amdf_status_t status = api_->memory_map(memory, &map_info, out_mapping);
    if (!amdf_status_is_ok(status)) return status;
    out_info->type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    out_info->structure_size = sizeof(*out_info);
    return api_->host_mapping_query_info(*out_mapping, out_info);
  }

  const amdf_api_t* api_ = nullptr;
  const amdf_gpu_api_t* gpu_api_ = nullptr;
  const amdf_xdna_api_t* xdna_api_ = nullptr;
  amdf_instance_t* instance_ = nullptr;
  amdf_endpoint_t* gpu_endpoint_ = nullptr;
  amdf_endpoint_t* xdna_endpoint_ = nullptr;
  amdf_device_t* gpu_device_ = nullptr;
  amdf_device_t* xdna_device_ = nullptr;
  amdf_memory_t* gpu_memory_ = nullptr;
  amdf_memory_t* xdna_memory_ = nullptr;
  amdf_host_mapping_t* gpu_mapping_ = nullptr;
  amdf_host_mapping_t* xdna_mapping_ = nullptr;
  amdf_external_memory_t external_memory_ = {};
};

TEST_F(GpuXdnaMemoryInteropTest, ImportsGpuSubrangeAndSurvivesSourceTeardown) {
  const long system_page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(system_page_size, 0);
  const uint64_t page_size = static_cast<uint64_t>(system_page_size);
  const uint64_t backing_byte_length = page_size * 3;

  constexpr amdf_memory_flags_t kGpuRequiredFlags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_SHAREABLE |
      AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  uint32_t gpu_profile_ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
  amdf_memory_profile_t gpu_profile = {};
  ASSERT_EQ(
      FindDmaBufProfile(api_, gpu_device_,
                        AMDF_MEMORY_PROFILE_ROLE_CREATE |
                            AMDF_MEMORY_PROFILE_ROLE_EXPORT |
                            AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
                        kGpuRequiredFlags,
                        AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                            AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET,
                        &gpu_profile_ordinal, &gpu_profile),
      AMDF_STATUS_OK);
  ASSERT_NE(gpu_profile_ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
  const amdf_external_memory_support_t* gpu_dma_buf_support = FindDmaBufSupport(
      gpu_profile, AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                       AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET);
  ASSERT_NE(gpu_dma_buf_support, nullptr);
  ASSERT_NE(gpu_dma_buf_support->source_offset_alignment, 0u);
  ASSERT_EQ(page_size % gpu_dma_buf_support->source_offset_alignment, 0u);
  ASSERT_NE(gpu_dma_buf_support->byte_length_alignment, 0u);
  ASSERT_EQ(page_size % gpu_dma_buf_support->byte_length_alignment, 0u);

  constexpr amdf_memory_flags_t kXdnaRequiredFlags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  uint32_t xdna_profile_ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
  amdf_memory_profile_t xdna_profile = {};
  ASSERT_EQ(
      FindDmaBufProfile(
          api_, xdna_device_,
          AMDF_MEMORY_PROFILE_ROLE_IMPORT | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
          kXdnaRequiredFlags,
          AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
              AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
              AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_FOREIGN_API,
          &xdna_profile_ordinal, &xdna_profile),
      AMDF_STATUS_OK);
  ASSERT_NE(xdna_profile_ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);

  amdf_memory_create_info_t create_info = {};
  create_info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  create_info.structure_size = sizeof(create_info);
  create_info.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
  create_info.required_flags = kGpuRequiredFlags;
  create_info.byte_length = backing_byte_length;
  create_info.minimum_alignment = page_size;
  ASSERT_EQ(api_->memory_create(gpu_device_, &create_info, &gpu_memory_),
            AMDF_STATUS_OK);

  amdf_memory_info_t gpu_memory_info = {};
  gpu_memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  gpu_memory_info.structure_size = sizeof(gpu_memory_info);
  ASSERT_EQ(api_->memory_query_info(gpu_memory_, &gpu_memory_info),
            AMDF_STATUS_OK);
  EXPECT_EQ(gpu_memory_info.memory_profile_ordinal, gpu_profile_ordinal);
  EXPECT_EQ(gpu_memory_info.flags & kGpuRequiredFlags, kGpuRequiredFlags);
  ASSERT_GE(gpu_memory_info.byte_length, backing_byte_length);
  ASSERT_TRUE(
      amdf_physical_memory_id_is_valid(&gpu_memory_info.physical_backing_id));

  amdf_host_mapping_info_t gpu_mapping_info = {};
  ASSERT_EQ(Map(gpu_memory_, gpu_memory_info.byte_length, &gpu_mapping_,
                &gpu_mapping_info),
            AMDF_STATUS_OK);
  auto* gpu_bytes = static_cast<uint8_t*>(gpu_mapping_info.pointer);
  ASSERT_NE(gpu_bytes, nullptr);
  std::memset(gpu_bytes, 0x11, static_cast<size_t>(page_size));
  std::memset(gpu_bytes + page_size, 0x22, static_cast<size_t>(page_size));
  std::memset(gpu_bytes + 2 * page_size, 0x33, static_cast<size_t>(page_size));
  ASSERT_EQ(api_->host_mapping_cache_control(gpu_mapping_,
                                             AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                                             backing_byte_length),
            AMDF_STATUS_OK);

  amdf_memory_export_info_t export_info = {};
  export_info.type = AMDF_STRUCTURE_TYPE_MEMORY_EXPORT_INFO;
  export_info.structure_size = sizeof(export_info);
  export_info.external_memory_type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  export_info.byte_offset = page_size;
  export_info.byte_length = page_size;
  ASSERT_EQ(api_->memory_export(gpu_memory_, &export_info, &external_memory_),
            AMDF_STATUS_OK);
  ASSERT_EQ(external_memory_.type, AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);
  ASSERT_GE(external_memory_.payload.file_descriptor, 0);
  ASSERT_LE(external_memory_.payload.file_descriptor, INT32_MAX);
  const int exported_descriptor =
      static_cast<int>(external_memory_.payload.file_descriptor);
  const int descriptor_flags = fcntl(exported_descriptor, F_GETFD);
  ASSERT_NE(descriptor_flags, -1);
  EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);
  EXPECT_EQ(external_memory_.source_byte_offset, page_size);
  EXPECT_EQ(external_memory_.byte_length, page_size);
  EXPECT_TRUE(
      amdf_physical_memory_id_is_equal(&external_memory_.physical_backing_id,
                                       &gpu_memory_info.physical_backing_id));

  amdf_memory_import_info_t import_info = {};
  import_info.type = AMDF_STRUCTURE_TYPE_MEMORY_IMPORT_INFO;
  import_info.structure_size = sizeof(import_info);
  import_info.memory_profile_ordinal = xdna_profile_ordinal;
  import_info.required_flags = kXdnaRequiredFlags;
  import_info.minimum_alignment = page_size;
  ASSERT_EQ(api_->memory_import(xdna_device_, &import_info, &external_memory_,
                                &xdna_memory_),
            AMDF_STATUS_OK);
  const amdf_external_memory_t empty_external_memory = {};
  EXPECT_EQ(std::memcmp(&external_memory_, &empty_external_memory,
                        sizeof(external_memory_)),
            0);
  errno = 0;
  EXPECT_EQ(fcntl(exported_descriptor, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);

  amdf_memory_info_t xdna_memory_info = {};
  xdna_memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  xdna_memory_info.structure_size = sizeof(xdna_memory_info);
  ASSERT_EQ(api_->memory_query_info(xdna_memory_, &xdna_memory_info),
            AMDF_STATUS_OK);
  EXPECT_EQ(xdna_memory_info.memory_profile_ordinal, xdna_profile_ordinal);
  EXPECT_EQ(xdna_memory_info.flags & kXdnaRequiredFlags, kXdnaRequiredFlags);
  EXPECT_EQ(xdna_memory_info.source_byte_offset, page_size);
  EXPECT_EQ(xdna_memory_info.byte_length, page_size);
  EXPECT_GE(xdna_memory_info.alignment, page_size);
  EXPECT_EQ(xdna_memory_info.device_address & (page_size - 1), 0u);
  EXPECT_TRUE(
      amdf_physical_memory_id_is_equal(&xdna_memory_info.physical_backing_id,
                                       &gpu_memory_info.physical_backing_id));

  amdf_host_mapping_info_t xdna_mapping_info = {};
  ASSERT_EQ(Map(xdna_memory_, page_size, &xdna_mapping_, &xdna_mapping_info),
            AMDF_STATUS_OK);
  auto* xdna_bytes = static_cast<uint8_t*>(xdna_mapping_info.pointer);
  ASSERT_NE(xdna_bytes, nullptr);
  ASSERT_EQ(
      api_->host_mapping_cache_control(
          xdna_mapping_, AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0, page_size),
      AMDF_STATUS_OK);
  EXPECT_EQ(xdna_bytes[0], 0x22);
  EXPECT_EQ(xdna_bytes[page_size - 1], 0x22);

  std::memset(xdna_bytes, 0xC3, static_cast<size_t>(page_size));
  ASSERT_EQ(api_->host_mapping_cache_control(
                xdna_mapping_, AMDF_HOST_CACHE_OPERATION_FLUSH, 0, page_size),
            AMDF_STATUS_OK);
  ASSERT_EQ(api_->host_mapping_cache_control(
                gpu_mapping_, AMDF_HOST_CACHE_OPERATION_INVALIDATE, page_size,
                page_size),
            AMDF_STATUS_OK);
  EXPECT_EQ(gpu_bytes[0], 0x11);
  EXPECT_EQ(gpu_bytes[page_size], 0xC3);
  EXPECT_EQ(gpu_bytes[2 * page_size - 1], 0xC3);
  EXPECT_EQ(gpu_bytes[2 * page_size], 0x33);

  ASSERT_EQ(api_->host_mapping_destroy(gpu_mapping_), AMDF_STATUS_OK);
  gpu_mapping_ = nullptr;
  ASSERT_EQ(api_->memory_destroy(gpu_memory_), AMDF_STATUS_OK);
  gpu_memory_ = nullptr;
  ASSERT_EQ(api_->device_destroy(gpu_device_), AMDF_STATUS_OK);
  gpu_device_ = nullptr;
  ASSERT_EQ(api_->endpoint_close(gpu_endpoint_), AMDF_STATUS_OK);
  gpu_endpoint_ = nullptr;

  ASSERT_EQ(
      api_->host_mapping_cache_control(
          xdna_mapping_, AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0, page_size),
      AMDF_STATUS_OK);
  EXPECT_EQ(xdna_bytes[0], 0xC3);
  EXPECT_EQ(xdna_bytes[page_size - 1], 0xC3);
  xdna_bytes[page_size / 2] = 0x7D;
  EXPECT_EQ(xdna_bytes[page_size / 2], 0x7D);
}

TEST_F(GpuXdnaMemoryInteropTest,
       RejectsMismatchedIdentityWithoutConsumingTransport) {
  const long system_page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(system_page_size, 0);
  const uint64_t page_size = static_cast<uint64_t>(system_page_size);

  uint32_t xdna_profile_ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
  amdf_memory_profile_t xdna_profile = {};
  ASSERT_EQ(
      FindDmaBufProfile(
          api_, xdna_device_,
          AMDF_MEMORY_PROFILE_ROLE_IMPORT | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
          AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
          AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
              AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_FOREIGN_API,
          &xdna_profile_ordinal, &xdna_profile),
      AMDF_STATUS_OK);
  ASSERT_NE(xdna_profile_ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);

  amdf_memory_create_info_t create_info = {};
  create_info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  create_info.structure_size = sizeof(create_info);
  create_info.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
  create_info.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                               AMDF_MEMORY_FLAG_SHAREABLE |
                               AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  create_info.byte_length = page_size;
  ASSERT_EQ(api_->memory_create(gpu_device_, &create_info, &gpu_memory_),
            AMDF_STATUS_OK);

  amdf_memory_export_info_t export_info = {};
  export_info.type = AMDF_STRUCTURE_TYPE_MEMORY_EXPORT_INFO;
  export_info.structure_size = sizeof(export_info);
  export_info.external_memory_type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  export_info.byte_length = page_size;
  ASSERT_EQ(api_->memory_export(gpu_memory_, &export_info, &external_memory_),
            AMDF_STATUS_OK);
  ASSERT_TRUE(
      amdf_physical_memory_id_is_valid(&external_memory_.physical_backing_id));
  external_memory_.physical_backing_id.words[0] ^= UINT64_C(1);
  const amdf_external_memory_t mismatched_external_memory = external_memory_;
  ASSERT_GE(external_memory_.payload.file_descriptor, 0);
  ASSERT_LE(external_memory_.payload.file_descriptor, INT32_MAX);
  const int descriptor =
      static_cast<int>(external_memory_.payload.file_descriptor);

  amdf_memory_import_info_t import_info = {};
  import_info.type = AMDF_STRUCTURE_TYPE_MEMORY_IMPORT_INFO;
  import_info.structure_size = sizeof(import_info);
  import_info.memory_profile_ordinal = xdna_profile_ordinal;
  import_info.required_flags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* output = sentinel;
  EXPECT_EQ(amdf_status_code(api_->memory_import(xdna_device_, &import_info,
                                                 &external_memory_, &output)),
            AMDF_STATUS_CODE_FAILED_PRECONDITION);
  EXPECT_EQ(output, sentinel);
  EXPECT_EQ(std::memcmp(&external_memory_, &mismatched_external_memory,
                        sizeof(external_memory_)),
            0);
  EXPECT_NE(fcntl(descriptor, F_GETFD), -1);
}

}  // namespace
