// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "experimental/xdna/executable.h"
#include "experimental/xdna/prepared_command.h"
#include "iree/hal/drivers/amd/xdna/image/aie2p/npu2.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/mul_i32.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/mul_i32_npu4.h"
#include "iree/hal/drivers/amd/xdna/image/testing/aie2p_image_fixture.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "libamdf/cts/xdna/xdna_device_fixture.h"

namespace {

// Both canonical compiler fixtures multiply sixteen low-32-bit integer pairs.
constexpr size_t kElementCount = 16;
constexpr size_t kBindingByteLength = kElementCount * sizeof(uint32_t);

class XdnaExecutionTest : public XdnaContextFixture {
 protected:
  struct MappedMemory {
    // Case-owned allocation; its context and device outlive it.
    amdf_memory_t* memory = nullptr;
    // Explicit host mapping, destroyed before the allocation.
    amdf_host_mapping_t* mapping = nullptr;
    // Borrowed host view of the mapped range.
    uint8_t* pointer = nullptr;
  };

  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(XdnaContextFixture::SetUp());
    if (IsSkipped()) return;
    amdf_xdna_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    ASSERT_EQ(xdna_api_->endpoint_query_info(endpoint_, &info), AMDF_STATUS_OK);
    instruction_alignment_ = info.instruction.address_alignment;

    const iree_file_toc_t* image = nullptr;
    if (std::strcmp(info.target_id, "amd.xdna.strix.17f0_10") == 0) {
      image = iree_hal_amd_xdna_test_mul_i32_npu4_create();
    } else if (std::strcmp(info.target_id, "amd.xdna.strix_halo.17f0_11") ==
               0) {
      image = iree_hal_amd_xdna_test_mul_i32_create();
    } else {
      GTEST_SKIP() << "no canonical multiplication fixture for "
                   << info.target_id;
    }
    amdf_endpoint_info_t endpoint_info = {};
    endpoint_info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
    endpoint_info.structure_size = sizeof(endpoint_info);
    ASSERT_EQ(api_->endpoint_query_info(endpoint_, &endpoint_info),
              AMDF_STATUS_OK);
    uint32_t family_ordinal = UINT32_MAX;
    for (uint32_t i = 0; i < endpoint_info.queue_family_count; ++i) {
      amdf_queue_family_info_t family = {};
      family.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
      family.structure_size = sizeof(family);
      ASSERT_EQ(api_->endpoint_query_queue_family_info(endpoint_, i, &family),
                AMDF_STATUS_OK);
      if (family.command_type == AMDF_QUEUE_COMMAND_TYPE_XDNA &&
          (family.publication_modes & AMDF_QUEUE_PUBLICATION_MODE_KERNEL) !=
              0) {
        family_ordinal = i;
        break;
      }
    }
    ASSERT_NE(family_ordinal, UINT32_MAX);
    queue_family_spec_.name = IREE_SV("xdna");
    queue_family_spec_.physical_device_affinity = 1;
    queue_family_spec_.role_flags = IREE_HAL_QUEUE_FAMILY_ROLE_FLAG_DISPATCH;
    iree_hal_queue_family_initialize(family_ordinal, &queue_family_spec_,
                                     &queue_family_);
    iree_hal_amd_xdna_aie2p_target_t target;
    IREE_ASSERT_OK(iree_hal_amd_xdna_aie2p_npu2_target_initialize(
        iree_make_cstring_view(info.target_id), 1, &target));
    const auto* image_bytes = reinterpret_cast<const uint8_t*>(image->data);
    auto sequence = iree::hal::amd::xdna::testing::MakeOwnedByteSequence(
        std::vector<uint8_t>(image_bytes, image_bytes + image->size));
    IREE_ASSERT_OK(iree_hal_amd_xdna_executable_create(
        &queue_family_, sequence.get(), &target, iree_allocator_system(),
        &executable_));
    IREE_ASSERT_OK(iree_hal_executable_lookup_function_by_name(
        executable_, IREE_SV("mul_i32"), &function_));
  }

  void DestroyMemory(MappedMemory* memory) {
    if (memory->mapping) {
      ASSERT_EQ(api_->host_mapping_destroy(memory->mapping), AMDF_STATUS_OK);
      memory->mapping = nullptr;
    }
    if (memory->memory) {
      ASSERT_EQ(api_->memory_destroy(memory->memory), AMDF_STATUS_OK);
      memory->memory = nullptr;
    }
  }

  void TearDown() override {
    if (queue_) {
      ASSERT_EQ(api_->kernel_queue_destroy(queue_), AMDF_STATUS_OK);
      queue_ = nullptr;
    }
    iree_hal_amd_xdna_prepared_command_destroy(prepared_);
    prepared_ = nullptr;
    ASSERT_NO_FATAL_FAILURE(DestroyMemory(&instructions_));
    for (auto& binding : bindings_) {
      iree_hal_buffer_release(binding.buffer);
      binding.buffer = nullptr;
      ASSERT_NO_FATAL_FAILURE(DestroyMemory(&binding.storage));
    }
    iree_hal_executable_release(executable_);
    executable_ = nullptr;
    XdnaContextFixture::TearDown();
  }

  void MapMemory(uint64_t byte_length, MappedMemory* memory) {
    amdf_memory_map_info_t map = {};
    map.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
    map.structure_size = sizeof(map);
    map.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
    map.byte_length = byte_length;
    ASSERT_EQ(api_->memory_map(memory->memory, &map, &memory->mapping),
              AMDF_STATUS_OK);
    amdf_host_mapping_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    info.structure_size = sizeof(info);
    ASSERT_EQ(api_->host_mapping_query_info(memory->mapping, &info),
              AMDF_STATUS_OK);
    memory->pointer = static_cast<uint8_t*>(info.pointer);
  }

  void CreateBindings() {
    memory_access_.requirements.address_kinds = uint64_t{1}
                                                << AMDF_MEMORY_ADDRESS_XDNA_DMA;
    const uint32_t profile_ordinal = FindMemoryProfileOrdinal(
        AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
        AMDF_MEMORY_FLAG_HOST_VISIBLE);
    ASSERT_NE(profile_ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
    for (size_t i = 0; i < bindings_.size(); ++i) {
      auto& binding = bindings_[i];
      amdf_memory_create_info_t create = {};
      create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
      create.structure_size = sizeof(create);
      create.memory_profile_ordinal = profile_ordinal;
      create.access_count = 1;
      create.accesses = &memory_access_;
      create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
      create.byte_length = kBindingByteLength;
      ASSERT_EQ(
          api_->memory_create(system_scope_, &create, &binding.storage.memory),
          AMDF_STATUS_OK);
      ASSERT_NO_FATAL_FAILURE(MapMemory(kBindingByteLength, &binding.storage));
      IREE_ASSERT_OK(iree_hal_heap_buffer_wrap(
          iree_hal_buffer_placement_undefined(),
          IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_VISIBLE |
              IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE,
          IREE_HAL_MEMORY_ACCESS_READ | IREE_HAL_MEMORY_ACCESS_WRITE |
              IREE_HAL_MEMORY_ACCESS_UNALIGNED,
          IREE_HAL_BUFFER_USAGE_STORAGE, kBindingByteLength,
          iree_make_byte_span(binding.storage.pointer, kBindingByteLength),
          iree_hal_buffer_release_callback_null(), iree_allocator_system(),
          &binding.buffer));
      prepared_bindings_[i].buffer_ref =
          iree_hal_make_buffer_ref(binding.buffer, 0, kBindingByteLength);
      prepared_bindings_[i].memory = binding.storage.memory;
      ASSERT_EQ(api_->memory_query_address(
                    binding.storage.memory, 0, AMDF_MEMORY_ADDRESS_XDNA_DMA,
                    &prepared_bindings_[i].device_address),
                AMDF_STATUS_OK);
    }
  }

  void PrepareInstructions() {
    IREE_ASSERT_OK(iree_hal_amd_xdna_prepared_command_query_storage_size(
        executable_, function_, instruction_alignment_,
        &instruction_byte_length_));
    amdf_memory_scope_t* scope = nullptr;
    uint32_t count = 0;
    ASSERT_EQ(
        xdna_api_->context_enumerate_memory_scopes(context_, 1, &scope, &count),
        AMDF_STATUS_OK);
    amdf_memory_device_access_t access = {};
    access.device = device_;
    access.requirements.access = AMDF_MEMORY_ACCESS_READ |
                                 AMDF_MEMORY_ACCESS_WRITE |
                                 AMDF_MEMORY_ACCESS_EXECUTE;
    access.requirements.flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    access.requirements.address_kinds = uint64_t{1}
                                        << AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE;
    amdf_memory_profile_t profile = {};
    profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
    profile.structure_size = sizeof(profile);
    amdf_memory_access_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    ASSERT_EQ(api_->memory_scope_query_device_profile(scope, 0, 1, &access,
                                                      &profile, &capabilities),
              AMDF_STATUS_OK);
    const uint64_t granularity = profile.allocation.byte_length_granularity;
    ASSERT_GT(granularity, 0u);
    amdf_memory_create_info_t create = {};
    create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create.structure_size = sizeof(create);
    create.memory_profile_ordinal = profile.ordinal;
    create.access_count = 1;
    create.accesses = &access;
    create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
    create.byte_length =
        ((instruction_byte_length_ + granularity - 1) / granularity) *
        granularity;
    create.minimum_alignment = profile.allocation.minimum_alignment;
    ASSERT_EQ(api_->memory_create(scope, &create, &instructions_.memory),
              AMDF_STATUS_OK);
    uint64_t firmware_address = 0;
    ASSERT_EQ(api_->memory_query_address(instructions_.memory, 0,
                                         AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE,
                                         &firmware_address),
              AMDF_STATUS_OK);
    ASSERT_EQ(firmware_address % instruction_alignment_, 0u);
    ASSERT_NO_FATAL_FAILURE(
        MapMemory(instruction_byte_length_, &instructions_));
    amdf_xdna_kernel_command_t storage = {};
    storage.memory = instructions_.memory;
    storage.byte_length = instruction_byte_length_;
    IREE_ASSERT_OK(iree_hal_amd_xdna_prepared_command_create(
        executable_, function_, instruction_alignment_, &storage,
        iree_make_byte_span(instructions_.pointer, instruction_byte_length_),
        prepared_bindings_.size(), prepared_bindings_.data(),
        iree_allocator_system(), &prepared_));
    ASSERT_EQ(api_->host_mapping_cache_control(instructions_.mapping,
                                               AMDF_HOST_CACHE_OPERATION_FLUSH,
                                               0, instruction_byte_length_),
              AMDF_STATUS_OK);
    amdf_xdna_kernel_queue_create_info_t queue_create = {};
    queue_create.type = AMDF_STRUCTURE_TYPE_XDNA_KERNEL_QUEUE_CREATE_INFO;
    queue_create.structure_size = sizeof(queue_create);
    queue_create.queue_family_ordinal = queue_family_.ordinal;
    ASSERT_EQ(xdna_api_->kernel_queue_create(context_, &queue_create, &queue_),
              AMDF_STATUS_OK);
  }

  // Executable-only family metadata; native queues are managed through libamdf.
  iree_hal_queue_family_spec_t queue_family_spec_ = {};
  // HAL queue-family descriptor borrowed by the executable.
  iree_hal_queue_family_t queue_family_ = {};
  // Case-owned immutable decoded compiler image.
  iree_hal_executable_t* executable_ = nullptr;
  // Reflected multiplication entry in executable_.
  iree_hal_executable_function_t function_ =
      iree_hal_executable_function_invalid();
  // Storage requirements derived from the endpoint and executable.
  iree_host_size_t instruction_alignment_ = 0;
  // Mapped length covering initialization and execution ranges.
  iree_host_size_t instruction_byte_length_ = 0;
  // One private instruction allocation; the caller owns its lifetime.
  MappedMemory instructions_;
  // Immutable command ranges retaining only the executable and HAL buffers.
  iree_hal_amd_xdna_prepared_command_t* prepared_ = nullptr;
  // Native queue borrowing this case's context.
  amdf_kernel_queue_t* queue_ = nullptr;
  // Native and HAL owners for the three canonical bindings.
  struct Binding {
    // Native memory and its explicit host view.
    MappedMemory storage;
    // HAL wrapper borrowing storage until preparation has been destroyed.
    iree_hal_buffer_t* buffer = nullptr;
  };
  // Lhs, rhs and output backing, independent of instruction storage.
  std::array<Binding, 3> bindings_;
  // Cold DMA addresses resolved through the public memory handles.
  std::array<iree_hal_amd_xdna_prepared_command_binding_t, 3>
      prepared_bindings_ = {};
};

TEST_F(XdnaExecutionTest, ReusesImmutableInstructionsWithChangingInputs) {
  ASSERT_NO_FATAL_FAILURE(CreateBindings());
  ASSERT_NO_FATAL_FAILURE(PrepareInstructions());
  const std::vector<uint8_t> original_instructions(
      instructions_.pointer, instructions_.pointer + instruction_byte_length_);
  // Unsigned arithmetic gives the exact low bits for signed and overflowing
  // products without invoking host signed-overflow undefined behavior.
  constexpr std::array<uint32_t, kElementCount> values = {
      0,          1,          2,          3,          7,          31,
      65535,      65536,      0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFD,
      0xFFFFFFFE, 0xFFFFFFFF, 0x12345678, 0x87654321};
  for (uint32_t iteration = 0; iteration < 3; ++iteration) {
    SCOPED_TRACE(iteration);
    std::array<uint32_t, kElementCount> expected;
    for (size_t i = 0; i < kElementCount; ++i) {
      const uint32_t lhs = values[(i + iteration) % kElementCount];
      const uint32_t rhs = values[(i * 3 + iteration + 5) % kElementCount];
      expected[i] = lhs * rhs;
      iree_unaligned_store_le_u32(bindings_[0].storage.pointer + i * 4, lhs);
      iree_unaligned_store_le_u32(bindings_[1].storage.pointer + i * 4, rhs);
      // Every output must change; neither zero-fill nor stale output can pass.
      iree_unaligned_store_le_u32(bindings_[2].storage.pointer + i * 4,
                                  ~expected[i]);
    }
    for (auto& binding : bindings_) {
      ASSERT_EQ(api_->host_mapping_cache_control(
                    binding.storage.mapping, AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                    kBindingByteLength),
                AMDF_STATUS_OK);
    }
    amdf_xdna_kernel_queue_submission_info_t submit = {};
    submit.type = AMDF_STRUCTURE_TYPE_XDNA_KERNEL_QUEUE_SUBMISSION_INFO;
    submit.structure_size = sizeof(submit);
    submit.command_count = 1;
    submit.commands =
        iteration == 0
            ? iree_hal_amd_xdna_prepared_command_initialization(prepared_)
            : iree_hal_amd_xdna_prepared_command_execution(prepared_);
    uint64_t submission = 0;
    ASSERT_EQ(xdna_api_->kernel_queue_submit(queue_, &submit, &submission),
              AMDF_STATUS_OK);
    ASSERT_EQ(
        api_->kernel_queue_wait(queue_, submission, AMDF_TIMEOUT_INFINITE, 0),
        AMDF_STATUS_OK);
    amdf_kernel_queue_status_t status = {};
    status.type = AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_STATUS;
    status.structure_size = sizeof(status);
    ASSERT_EQ(api_->kernel_queue_query_status(queue_, &status), AMDF_STATUS_OK);
    ASSERT_EQ(status.retired_submission, submission);
    ASSERT_EQ(status.terminal_status, AMDF_STATUS_OK);
    ASSERT_EQ(api_->host_mapping_cache_control(
                  bindings_[2].storage.mapping,
                  AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0, kBindingByteLength),
              AMDF_STATUS_OK);
    for (size_t i = 0; i < kElementCount; ++i) {
      EXPECT_EQ(
          iree_unaligned_load_le_u32(bindings_[2].storage.pointer + i * 4),
          expected[i])
          << "element " << i;
    }
    ASSERT_EQ(api_->host_mapping_cache_control(
                  instructions_.mapping, AMDF_HOST_CACHE_OPERATION_INVALIDATE,
                  0, instruction_byte_length_),
              AMDF_STATUS_OK);
    ASSERT_EQ(std::memcmp(original_instructions.data(), instructions_.pointer,
                          original_instructions.size()),
              0);
  }
}

}  // namespace
