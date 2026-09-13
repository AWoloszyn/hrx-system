// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "amdf/amdf.h"
#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"
#include "libamdf/src/child_tracker.h"
#include "libamdf/src/device.h"

namespace {

struct ReleaseState {
  // Number of external ownership obligations consumed.
  uint32_t count;
  // Representation passed to the last release.
  amdf_external_memory_type_t type;
  // Exact payload passed to the last release.
  amdf_external_memory_payload_t payload;
};

static void AMDF_CALL RecordRelease(void* user_data,
                                    amdf_external_memory_type_t type,
                                    amdf_external_memory_payload_t payload) {
  auto* state = static_cast<ReleaseState*>(user_data);
  ++state->count;
  state->type = type;
  state->payload = payload;
}

enum class ImportFailureStage {
  kNone,
  kBeforeAttachment,
  kAfterAttachment,
};

struct FakeDevice {
  // Real common device dependency used by the API under test.
  amdf_device_t base;
  // Complete capabilities returned by the device dependency.
  amdf_memory_profile_t profile;
  // Consumer addresses established by the fake native construction boundary.
  std::array<uint64_t, AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE + 1> addresses;
  // Injected query result before profile publication.
  amdf_status_t profile_status;
  // Injected allocation result after native metadata is acquired.
  amdf_status_t create_status;
  // Injected import result at import_failure_stage.
  amdf_status_t import_status;
  // Import prefix completed before reporting the injected error.
  ImportFailureStage import_failure_stage;
  // Whether the native importer needs the input lease until destruction.
  bool adopt_external_memory;
  // Number of calls into native allocation preparation.
  uint32_t create_call_count;
  // Number of calls into native import preparation.
  uint32_t import_call_count;
  // Native destruction status copied into each prepared allocation.
  amdf_status_t destroy_status;
  // Number of native destruction attempts.
  uint32_t destroy_call_count;
  // Number of metadata-only abandonments after terminal native failure.
  uint32_t abandon_call_count;
  // Native backing identity reported by allocation.
  amdf_physical_memory_id_t backing_id;
};

struct FakeMemory {
  // Device dependency outliving this native allocation.
  FakeDevice* device;
  // Injected export result before publishing a payload.
  amdf_status_t export_status;
  // Injected host-mapping result.
  amdf_status_t map_status;
  // Injected site-query result before publication.
  amdf_status_t site_status;
  // Native release result injected before relinquishing backing ownership.
  amdf_status_t destroy_status;
  // Number of export calls on this native object.
  uint32_t export_call_count;
  // Number of host-mapping calls on this native object.
  uint32_t map_call_count;
  // Number of site queries on this native object.
  uint32_t site_description_count;
  // Queue family received by the last site query.
  uint32_t last_queue_family_ordinal;
  // Complete native facts returned by site queries.
  amdf_memory_site_description_t site_description;
  // Borrowed observer of ownership exported by this native object.
  ReleaseState* export_release_state;
  // Input lease committed by common construction, empty until success.
  amdf_external_memory_t adopted_external_memory;
};

static amdf_status_t FakeMemoryExport(
    amdf_memory_t* base_memory, const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value) {
  (void)export_info;
  auto* memory = static_cast<FakeMemory*>(base_memory->native);
  ++memory->export_call_count;
  if (!amdf_status_is_ok(memory->export_status)) {
    return memory->export_status;
  }
  out_value->payload.file_descriptor = 73;
  out_value->release = RecordRelease;
  out_value->release_user_data = memory->export_release_state;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeMemoryDescribeSite(
    amdf_memory_t* base_memory, uint32_t queue_family_ordinal,
    amdf_memory_site_description_t* out_description) {
  auto* memory = static_cast<FakeMemory*>(base_memory->native);
  ++memory->site_description_count;
  memory->last_queue_family_ordinal = queue_family_ordinal;
  if (!amdf_status_is_ok(memory->site_status)) {
    return memory->site_status;
  }
  *out_description = memory->site_description;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeMemoryMap(amdf_memory_t* memory,
                                   const amdf_memory_profile_t* profile,
                                   const amdf_memory_map_info_t* map_info,
                                   amdf_host_mapping_t** out_mapping) {
  (void)profile;
  (void)map_info;
  (void)out_mapping;
  auto* fake_memory = static_cast<FakeMemory*>(memory->native);
  ++fake_memory->map_call_count;
  if (!amdf_status_is_ok(fake_memory->map_status)) {
    return fake_memory->map_status;
  }
  return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
}

static amdf_status_t FakeMemoryDestroyNative(amdf_memory_t* base_memory) {
  auto* memory = static_cast<FakeMemory*>(base_memory->native);
  ++memory->device->destroy_call_count;
  if (!amdf_status_is_ok(memory->destroy_status)) {
    return memory->destroy_status;
  }
  amdf_external_memory_release(&memory->adopted_external_memory);
  amdf_free(base_memory->host_allocator, memory);
  base_memory->native = nullptr;
  return AMDF_STATUS_OK;
}

static void FakeMemoryAbandonNative(amdf_memory_t* memory) {
  ++static_cast<FakeMemory*>(memory->native)->device->abandon_call_count;
  amdf_free(memory->host_allocator, memory->native);
  memory->native = nullptr;
}

static const amdf_memory_vtable_t kFakeMemoryVtable = {
    .export_external = FakeMemoryExport,
    .describe_site = FakeMemoryDescribeSite,
    .map = FakeMemoryMap,
    .destroy_native = FakeMemoryDestroyNative,
    .abandon_native = FakeMemoryAbandonNative,
};

static amdf_status_t PrepareFakeMemory(amdf_memory_t* base_memory,
                                       uint32_t memory_profile_ordinal,
                                       uint64_t source_byte_offset,
                                       uint64_t byte_length,
                                       amdf_physical_memory_id_t backing_id) {
  auto* device = reinterpret_cast<FakeDevice*>(base_memory->device);
  const amdf_allocator_t host_allocator =
      amdf_device_host_allocator(&device->base);
  FakeMemory* memory = nullptr;
  const amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*memory), amdf_alignof(FakeMemory),
                  reinterpret_cast<void**>(&memory));
  if (!amdf_status_is_ok(status)) return status;
  base_memory->native = memory;
  memory->device = device;
  memory->export_status = AMDF_STATUS_OK;
  memory->map_status = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  memory->site_status = AMDF_STATUS_OK;
  memory->destroy_status = device->destroy_status;
  memory->site_description.capabilities =
      AMDF_MEMORY_SITE_CAPABILITY_READ | AMDF_MEMORY_SITE_CAPABILITY_WRITE |
      AMDF_MEMORY_SITE_CAPABILITY_MAPPING_SOURCE |
      AMDF_MEMORY_SITE_CAPABILITY_MAPPING_TARGET |
      AMDF_MEMORY_SITE_CAPABILITY_RELEASE_COST_KNOWN |
      AMDF_MEMORY_SITE_CAPABILITY_ACQUIRE_COST_KNOWN;
  memory->site_description.release.kind = AMDF_CACHE_TRANSITION_KIND_NONE;
  memory->site_description.acquire.kind = AMDF_CACHE_TRANSITION_KIND_RANGE;
  memory->site_description.acquire.executor =
      AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE;
  memory->site_description.acquire.operation =
      AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM;
  memory->site_description.acquire.range_granularity = 64;
  memory->site_description.mapping_domain.words[0] = 1;
  memory->site_description.atomic_domain.words[0] = 2;
  memory->site_description.atomic_reach.scope_32 = AMDF_ATOMIC_SCOPE_SYSTEM;
  memory->site_description.atomic_reach.scope_64 = AMDF_ATOMIC_SCOPE_DEVICE;
  memory->site_description.release_fixed_cost_nanoseconds = 17;
  memory->site_description.acquire_fixed_cost_nanoseconds = 25;
  base_memory->info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  base_memory->info.structure_size = sizeof(base_memory->info);
  base_memory->info.memory_profile_ordinal = memory_profile_ordinal;
  base_memory->info.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
  base_memory->info.device_access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  base_memory->info.atomic_operations_32 = device->profile.atomic_operations_32;
  base_memory->info.atomic_operations_64 = device->profile.atomic_operations_64;
  base_memory->info.address_domain_ordinal = 0;
  base_memory->info.flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                            AMDF_MEMORY_FLAG_SHAREABLE |
                            AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  base_memory->info.source_byte_offset = source_byte_offset;
  base_memory->info.byte_length = byte_length;
  base_memory->info.alignment = 4096;
  base_memory->info.native_allocation_byte_length =
      source_byte_offset + byte_length;
  base_memory->info.native_allocation_granularity = 4096;
  base_memory->info.physical_backing_id = backing_id;
  std::memcpy(base_memory->addresses, device->addresses.data(),
              sizeof(base_memory->addresses));
  base_memory->info.address_kinds = device->profile.address_kinds;
  base_memory->info.reset_epoch = 1;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeDeviceQueryMemoryProfile(
    amdf_device_t* base_device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  auto* device = reinterpret_cast<FakeDevice*>(base_device);
  if (!amdf_status_is_ok(device->profile_status)) {
    return device->profile_status;
  }
  if (memory_profile_ordinal != device->profile.ordinal) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  *out_profile = device->profile;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeDeviceMemoryPrepare(
    amdf_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info) {
  auto* device = reinterpret_cast<FakeDevice*>(memory->device);
  memory->vtable = &kFakeMemoryVtable;
  ++device->create_call_count;
  const amdf_status_t status =
      PrepareFakeMemory(memory, profile->ordinal, 0, create_info->byte_length,
                        device->backing_id);
  return amdf_status_is_ok(status) ? device->create_status : status;
}

static amdf_status_t FakeDeviceMemoryPrepareImport(
    amdf_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_external_memory_t** out_external_memory_lease) {
  auto* device = reinterpret_cast<FakeDevice*>(memory->device);
  memory->vtable = &kFakeMemoryVtable;
  ++device->import_call_count;
  if (device->import_failure_stage == ImportFailureStage::kBeforeAttachment) {
    return device->import_status;
  }

  const amdf_status_t status = PrepareFakeMemory(
      memory, profile->ordinal, external_memory->source_byte_offset,
      external_memory->byte_length, external_memory->physical_backing_id);
  if (!amdf_status_is_ok(status)) return status;
  if (device->import_failure_stage == ImportFailureStage::kAfterAttachment) {
    return device->import_status;
  }
  auto* native = static_cast<FakeMemory*>(memory->native);
  *out_external_memory_lease = device->adopt_external_memory
                                   ? &native->adopted_external_memory
                                   : nullptr;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeDeviceDestroyNative(amdf_device_t* device) {
  (void)device;
  return AMDF_STATUS_OK;
}

static const amdf_device_vtable_t kFakeDeviceVtable = {
    .query_memory_profile = FakeDeviceQueryMemoryProfile,
    .memory_prepare = FakeDeviceMemoryPrepare,
    .memory_prepare_import = FakeDeviceMemoryPrepareImport,
    .destroy_native = FakeDeviceDestroyNative,
};

static void InitializeFakeDevice(uint64_t identity, FakeDevice* out_device) {
  std::memset(out_device, 0, sizeof(*out_device));
  out_device->base.host_allocator = amdf_allocator_system();
  out_device->base.vtable = &kFakeDeviceVtable;
  out_device->base.provider_instance =
      reinterpret_cast<amdf_instance_t*>(uintptr_t{1});
  out_device->base.engine_kind = AMDF_ENGINE_KIND_GPU;
  amdf_child_tracker_initialize(&out_device->base.children);
  out_device->profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  out_device->profile.structure_size = sizeof(out_device->profile);
  out_device->profile.ordinal = 0;
  out_device->profile.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
  out_device->profile.roles =
      AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_IMPORT |
      AMDF_MEMORY_PROFILE_ROLE_EXPORT | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
  out_device->profile.guaranteed_flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  out_device->profile.supported_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                                        AMDF_MEMORY_FLAG_SHAREABLE |
                                        AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  out_device->profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
  out_device->profile.supported_device_access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  out_device->profile.device_address.address_domain_ordinal = 0;
  out_device->profile.address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU;
  out_device->addresses[AMDF_MEMORY_ADDRESS_GPU] = UINT64_C(0x100000);
  out_device->profile.device_address.address_bit_count = 48;
  out_device->profile.device_address.maximum_address = (UINT64_C(1) << 48) - 1;
  out_device->profile.device_address.minimum_alignment = 4096;
  out_device->profile.allocation.maximum_byte_length = UINT64_C(1) << 32;
  out_device->profile.allocation.byte_length_granularity = 1;
  out_device->profile.allocation.minimum_alignment = 4096;
  out_device->profile.allocation.maximum_alignment = UINT64_C(1) << 30;
  out_device->profile.allocation.native_byte_length_granularity = 4096;
  out_device->profile.import = out_device->profile.allocation;
  out_device->profile.import.minimum_alignment = 1;
  out_device->profile.host_mapping.maximum_byte_length = UINT64_C(1) << 32;
  out_device->profile.host_mapping.byte_offset_granularity = 1;
  out_device->profile.host_mapping.byte_length_granularity = 1;
  out_device->profile.host_mapping.supported_access =
      AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  out_device->profile.external_memory_support_count = 1;
  out_device->profile.external_memory_support[0].type =
      AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  out_device->profile.external_memory_support[0].flags =
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET;
  out_device->profile.external_memory_support[0].source_offset_alignment = 4096;
  out_device->profile.external_memory_support[0].byte_length_alignment = 4096;
  out_device->profile_status = AMDF_STATUS_OK;
  out_device->create_status = AMDF_STATUS_OK;
  out_device->import_status =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  out_device->backing_id.words[0] = identity;
  out_device->backing_id.words[1] = identity ^ UINT64_C(0xA5A5A5A5);
}

static amdf_memory_create_info_t MakeMemoryCreateInfo() {
  amdf_memory_create_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  info.structure_size = sizeof(info);
  info.memory_profile_ordinal = 0;
  info.device_access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  info.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                        AMDF_MEMORY_FLAG_SHAREABLE |
                        AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  info.byte_length = 4096;
  info.minimum_alignment = 4096;
  return info;
}

static amdf_memory_import_info_t MakeMemoryImportInfo() {
  amdf_memory_import_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_MEMORY_IMPORT_INFO;
  info.structure_size = sizeof(info);
  info.memory_profile_ordinal = 0;
  info.device_access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  info.required_flags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  info.minimum_alignment = 4096;
  return info;
}

static amdf_memory_export_info_t MakeMemoryExportInfo() {
  amdf_memory_export_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_MEMORY_EXPORT_INFO;
  info.structure_size = sizeof(info);
  info.external_memory_type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  info.byte_length = 4096;
  return info;
}

static amdf_memory_site_t MakeMemorySite(amdf_memory_t* memory,
                                         uint32_t queue_family_ordinal) {
  amdf_memory_site_t site = {};
  site.type = AMDF_STRUCTURE_TYPE_MEMORY_SITE;
  site.structure_size = sizeof(site);
  site.memory = memory;
  site.queue_family_ordinal = queue_family_ordinal;
  return site;
}

struct AllocationState {
  // Allocation attempt to fail, or UINT32_MAX when all allocations succeed.
  uint32_t failure_ordinal = UINT32_MAX;
  // Number of allocation attempts through this host allocator.
  uint32_t allocation_count = 0;
  // Number of successful host allocations not yet freed.
  uint32_t live_count = 0;

  amdf_allocator_t allocator() {
    amdf_allocator_t value = {};
    value.user_data = this;
    value.allocate = [](void* user_data, uint64_t byte_length,
                        uint64_t minimum_alignment) -> void* {
      auto* state = static_cast<AllocationState*>(user_data);
      if (state->allocation_count++ == state->failure_ordinal) return nullptr;
      const amdf_allocator_t system = amdf_allocator_system();
      void* pointer =
          system.allocate(system.user_data, byte_length, minimum_alignment);
      if (pointer != nullptr) ++state->live_count;
      return pointer;
    };
    value.free = [](void* user_data, void* pointer) {
      auto* state = static_cast<AllocationState*>(user_data);
      EXPECT_GT(state->live_count, 0u);
      --state->live_count;
      const amdf_allocator_t system = amdf_allocator_system();
      system.free(system.user_data, pointer);
    };
    return value;
  }
};

TEST(MemoryConstructionTest, AllocationFailureHasNoNativeReleaseObligation) {
  for (uint32_t failure_ordinal : {0u, 1u}) {
    AllocationState allocations;
    allocations.failure_ordinal = failure_ordinal;
    FakeDevice device;
    InitializeFakeDevice(7, &device);
    device.base.host_allocator = allocations.allocator();
    const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
    auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
    amdf_memory_t* memory = sentinel;
    EXPECT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
              amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED));
    EXPECT_EQ(memory, sentinel);
    EXPECT_EQ(device.create_call_count, failure_ordinal);
    EXPECT_EQ(device.destroy_call_count, 0u);
    EXPECT_EQ(device.abandon_call_count, 0u);
    EXPECT_EQ(allocations.live_count, 0u);
  }
}

TEST(MemoryConstructionTest, FailedPrepareDiscardsUnpublishedNativeMetadata) {
  const amdf_status_t release_error =
      amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  for (amdf_status_t destroy_status : {AMDF_STATUS_OK, release_error}) {
    AllocationState allocations;
    FakeDevice device;
    InitializeFakeDevice(9, &device);
    device.base.host_allocator = allocations.allocator();
    device.create_status =
        amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
    device.destroy_status = destroy_status;
    const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
    auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
    amdf_memory_t* memory = sentinel;
    EXPECT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
              amdf_status_is_ok(destroy_status) ? device.create_status
                                                : destroy_status);
    EXPECT_EQ(memory, sentinel);
    EXPECT_EQ(device.destroy_call_count, 1u);
    EXPECT_EQ(device.abandon_call_count,
              amdf_status_is_ok(destroy_status) ? 0u : 1u);
    EXPECT_EQ(allocations.live_count, 0u);
  }
}

TEST(MemoryConstructionTest, FailedImportDoesNotAdoptInputLeaseOnReleaseError) {
  AllocationState allocations;
  FakeDevice device;
  InitializeFakeDevice(13, &device);
  device.base.host_allocator = allocations.allocator();
  device.import_failure_stage = ImportFailureStage::kAfterAttachment;
  device.destroy_status = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  device.adopt_external_memory = true;
  device.profile.external_memory_support[0].type =
      AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  ReleaseState release = {};
  alignas(4096) std::array<uint8_t, 4096> backing = {};
  amdf_external_memory_t external = {};
  external.type = AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  external.byte_length = backing.size();
  external.payload.host_pointer = backing.data();
  external.physical_backing_id = device.backing_id;
  external.release = RecordRelease;
  external.release_user_data = &release;
  const amdf_external_memory_t original = external;
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo();
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* memory = sentinel;
  EXPECT_EQ(amdf_memory_import(&device.base, &import_info, &external, &memory),
            device.destroy_status);
  EXPECT_EQ(memory, sentinel);
  EXPECT_EQ(std::memcmp(&external, &original, sizeof(external)), 0);
  EXPECT_EQ(release.count, 0u);
  EXPECT_EQ(device.destroy_call_count, 1u);
  EXPECT_EQ(device.abandon_call_count, 1u);
  EXPECT_EQ(allocations.live_count, 0u);
  amdf_external_memory_release(&external);
  EXPECT_EQ(release.count, 1u);
}

TEST(ExternalMemoryTest, ReleaseInvokesCallbackOnceAndZerosValue) {
  ReleaseState release_state = {};
  amdf_external_memory_t value = {};
  value.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  value.payload.file_descriptor = 19;
  value.source_byte_offset = 128;
  value.byte_length = 256;
  value.release = RecordRelease;
  value.release_user_data = &release_state;

  amdf_external_memory_release(&value);

  EXPECT_EQ(release_state.count, 1u);
  EXPECT_EQ(release_state.type, AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);
  EXPECT_EQ(release_state.payload.file_descriptor, 19);
  const amdf_external_memory_t empty = {};
  EXPECT_EQ(std::memcmp(&value, &empty, sizeof(value)), 0);

  amdf_external_memory_release(&value);
  EXPECT_EQ(release_state.count, 1u);
}

TEST(MemoryAddressTest, QueriesCachedAddressAndRejectsUnavailableConsumers) {
  FakeDevice device;
  InitializeFakeDevice(11, &device);
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  amdf_memory_t* memory = nullptr;
  ASSERT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            AMDF_STATUS_OK);
  uint64_t address = 0;
  EXPECT_EQ(
      amdf_memory_query_address(memory, AMDF_MEMORY_ADDRESS_GPU, &address),
      AMDF_STATUS_OK);
  EXPECT_EQ(address, UINT64_C(0x100000));
  for (amdf_memory_address_kind_t kind :
       {AMDF_MEMORY_ADDRESS_XDNA_DMA, AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE}) {
    EXPECT_EQ(
        amdf_status_code(amdf_memory_query_address(memory, kind, &address)),
        AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(address, UINT64_C(0x100000));
  }
  EXPECT_EQ(
      amdf_status_code(amdf_memory_query_address(memory, UINT32_MAX, &address)),
      AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(address, UINT64_C(0x100000));
  EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                nullptr, AMDF_MEMORY_ADDRESS_GPU, &address)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(address, UINT64_C(0x100000));
  EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                memory, AMDF_MEMORY_ADDRESS_GPU, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  const auto* native = static_cast<const FakeMemory*>(memory->native);
  EXPECT_EQ(native->map_call_count, 0u);
  EXPECT_EQ(native->export_call_count, 0u);
  EXPECT_EQ(device.create_call_count, 1u);
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST(MemoryAddressTest, KeepsDistinctConsumerAddressesIncludingZero) {
  FakeDevice device;
  InitializeFakeDevice(12, &device);
  device.profile.address_kinds =
      (UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_DMA) |
      (UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE);
  device.addresses[AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE] = 0;
  device.addresses[AMDF_MEMORY_ADDRESS_XDNA_DMA] = UINT64_C(0x80000000);
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  amdf_memory_t* memory = nullptr;
  ASSERT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            AMDF_STATUS_OK);
  // A published object cannot consult the provider again for cached addresses.
  device.profile_status = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  for (int iteration = 0; iteration < 2; ++iteration) {
    uint64_t address = UINT64_MAX;
    EXPECT_EQ(amdf_memory_query_address(
                  memory, AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE, &address),
              AMDF_STATUS_OK);
    EXPECT_EQ(address, 0u);
    EXPECT_EQ(amdf_memory_query_address(memory, AMDF_MEMORY_ADDRESS_XDNA_DMA,
                                        &address),
              AMDF_STATUS_OK);
    EXPECT_EQ(address, UINT64_C(0x80000000));
    EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                  memory, AMDF_MEMORY_ADDRESS_GPU, &address)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(address, UINT64_C(0x80000000));
  }
  const auto* native = static_cast<const FakeMemory*>(memory->native);
  EXPECT_EQ(native->map_call_count, 0u);
  EXPECT_EQ(native->export_call_count, 0u);
  EXPECT_EQ(device.create_call_count, 1u);
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST(MemoryExternalTest, CompletesProfileExportImportPairAndReverseTeardown) {
  FakeDevice source_device;
  FakeDevice destination_device;
  InitializeFakeDevice(11, &source_device);
  InitializeFakeDevice(29, &destination_device);

  amdf_memory_profile_t profile = {};
  profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  profile.structure_size = sizeof(profile);
  ASSERT_EQ(amdf_device_query_memory_profile(&source_device.base, 0, &profile),
            AMDF_STATUS_OK);
  EXPECT_EQ(profile.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
  ASSERT_EQ(profile.external_memory_support_count, 1u);
  EXPECT_EQ(profile.external_memory_support[0].type,
            AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);

  amdf_memory_t* source_memory = nullptr;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  ASSERT_EQ(
      amdf_memory_create(&source_device.base, &create_info, &source_memory),
      AMDF_STATUS_OK);
  ReleaseState transport_release = {};
  static_cast<FakeMemory*>(source_memory->native)->export_release_state =
      &transport_release;

  amdf_external_memory_t external_memory = {};
  const amdf_memory_export_info_t export_info = MakeMemoryExportInfo();
  ASSERT_EQ(amdf_memory_export(source_memory, &export_info, &external_memory),
            AMDF_STATUS_OK);
  EXPECT_EQ(external_memory.type, AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);
  EXPECT_EQ(external_memory.payload.file_descriptor, 73);
  EXPECT_EQ(external_memory.source_byte_offset, 0u);
  EXPECT_EQ(external_memory.byte_length, 4096u);
  EXPECT_TRUE(amdf_physical_memory_id_is_equal(
      &external_memory.physical_backing_id,
      &source_memory->info.physical_backing_id));

  amdf_memory_t* destination_memory = nullptr;
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo();
  ASSERT_EQ(amdf_memory_import(&destination_device.base, &import_info,
                               &external_memory, &destination_memory),
            AMDF_STATUS_OK);
  EXPECT_EQ(transport_release.count, 1u);
  const amdf_external_memory_t empty = {};
  EXPECT_EQ(std::memcmp(&external_memory, &empty, sizeof(external_memory)), 0);

  amdf_memory_info_t destination_info = {};
  destination_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  destination_info.structure_size = sizeof(destination_info);
  ASSERT_EQ(amdf_memory_query_info(destination_memory, &destination_info),
            AMDF_STATUS_OK);
  EXPECT_EQ(destination_info.memory_profile_ordinal, 0u);
  EXPECT_EQ(destination_info.source_byte_offset, 0u);
  EXPECT_EQ(destination_info.byte_length, 4096u);
  EXPECT_TRUE(amdf_physical_memory_id_is_equal(
      &destination_info.physical_backing_id,
      &source_memory->info.physical_backing_id));

  const amdf_memory_site_t producer_site = MakeMemorySite(source_memory, 3);
  const amdf_memory_site_t consumer_site =
      MakeMemorySite(destination_memory, 5);
  amdf_memory_pair_info_t pair_info = {};
  pair_info.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  pair_info.structure_size = sizeof(pair_info);
  ASSERT_EQ(
      amdf_memory_query_pair_info(&producer_site, &consumer_site, &pair_info),
      AMDF_STATUS_OK);
  EXPECT_EQ(pair_info.flags, AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE |
                                 AMDF_MEMORY_PAIR_FLAG_MAPPING_SOURCE |
                                 AMDF_MEMORY_PAIR_FLAG_FIXED_COST_KNOWN);
  EXPECT_EQ(pair_info.release.kind, AMDF_CACHE_TRANSITION_KIND_NONE);
  EXPECT_EQ(pair_info.acquire.executor, AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE);
  EXPECT_EQ(pair_info.acquire.range_granularity, 64u);
  EXPECT_EQ(pair_info.atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_SYSTEM);
  EXPECT_EQ(pair_info.atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_DEVICE);
  EXPECT_EQ(pair_info.estimated_fixed_cost_nanoseconds, 42u);
  EXPECT_EQ(static_cast<FakeMemory*>(source_memory->native)
                ->last_queue_family_ordinal,
            3u);
  EXPECT_EQ(static_cast<FakeMemory*>(destination_memory->native)
                ->last_queue_family_ordinal,
            5u);

  auto* destination_fake = static_cast<FakeMemory*>(destination_memory->native);
  destination_fake->site_description.mapping_domain.words[0] = 19;
  destination_fake->site_description.atomic_domain.words[0] = 23;
  pair_info = {};
  pair_info.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  pair_info.structure_size = sizeof(pair_info);
  ASSERT_EQ(
      amdf_memory_query_pair_info(&producer_site, &consumer_site, &pair_info),
      AMDF_STATUS_OK);
  EXPECT_EQ(pair_info.flags, AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE |
                                 AMDF_MEMORY_PAIR_FLAG_FIXED_COST_KNOWN);
  EXPECT_EQ(pair_info.atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_NONE);
  EXPECT_EQ(pair_info.atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_NONE);

  ASSERT_EQ(amdf_memory_destroy(source_memory), AMDF_STATUS_OK);
  ASSERT_EQ(amdf_memory_destroy(destination_memory), AMDF_STATUS_OK);
  EXPECT_EQ(transport_release.count, 1u);
}

TEST(MemoryExternalTest, AcceptsUnknownNumericDeviceAddressEnvelope) {
  FakeDevice device;
  InitializeFakeDevice(31, &device);
  device.profile.device_address.address_bit_count =
      AMDF_MEMORY_ADDRESS_BIT_COUNT_UNKNOWN;
  device.profile.device_address.minimum_address = 0;
  device.profile.device_address.maximum_address = 0;

  amdf_memory_profile_t profile = {};
  profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  profile.structure_size = sizeof(profile);
  ASSERT_EQ(amdf_device_query_memory_profile(&device.base, 0, &profile),
            AMDF_STATUS_OK);
  EXPECT_EQ(profile.device_address.address_bit_count,
            AMDF_MEMORY_ADDRESS_BIT_COUNT_UNKNOWN);

  amdf_memory_t* memory = nullptr;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  ASSERT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            AMDF_STATUS_OK);
  ASSERT_NE(memory, nullptr);
  EXPECT_EQ(memory->addresses[AMDF_MEMORY_ADDRESS_GPU], UINT64_C(0x100000));
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST(MemoryExternalTest, ImportFailureNeverConsumesInputOrPublishesOutput) {
  FakeDevice device;
  InitializeFakeDevice(41, &device);
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo();
  ReleaseState release_state = {};
  amdf_external_memory_t external_memory = {};
  external_memory.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  external_memory.payload.file_descriptor = 83;
  external_memory.source_byte_offset = 4096;
  external_memory.byte_length = 8192;
  external_memory.physical_backing_id = device.backing_id;
  external_memory.release = RecordRelease;
  external_memory.release_user_data = &release_state;
  const amdf_external_memory_t original_external_memory = external_memory;
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});

  for (ImportFailureStage stage : {ImportFailureStage::kBeforeAttachment,
                                   ImportFailureStage::kAfterAttachment}) {
    device.import_failure_stage = stage;
    amdf_memory_t* output = sentinel;
    EXPECT_EQ(amdf_memory_import(&device.base, &import_info, &external_memory,
                                 &output),
              device.import_status);
    EXPECT_EQ(output, sentinel);
    EXPECT_EQ(std::memcmp(&external_memory, &original_external_memory,
                          sizeof(external_memory)),
              0);
    EXPECT_EQ(release_state.count, 0u);
  }

  amdf_memory_import_info_t invalid_info = import_info;
  invalid_info.device_access |= UINT32_C(1) << 31;
  const uint32_t prior_import_call_count = device.import_call_count;
  amdf_memory_t* output = sentinel;
  EXPECT_EQ(amdf_status_code(amdf_memory_import(&device.base, &invalid_info,
                                                &external_memory, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(device.import_call_count, prior_import_call_count);
  EXPECT_EQ(output, sentinel);
  EXPECT_EQ(std::memcmp(&external_memory, &original_external_memory,
                        sizeof(external_memory)),
            0);
  EXPECT_EQ(release_state.count, 0u);
  amdf_external_memory_release(&external_memory);
  EXPECT_EQ(release_state.count, 1u);
}

TEST(MemoryExternalTest, ImportProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(47, &device);
  const amdf_memory_profile_t supported_profile = device.profile;
  const amdf_memory_import_info_t supported_import_info =
      MakeMemoryImportInfo();
  ReleaseState release_state = {};
  amdf_external_memory_t external_memory = {};
  external_memory.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  external_memory.payload.file_descriptor = 89;
  external_memory.source_byte_offset = 4096;
  external_memory.byte_length = 8192;
  external_memory.physical_backing_id = device.backing_id;
  external_memory.release = RecordRelease;
  external_memory.release_user_data = &release_state;
  const amdf_external_memory_t supported_external_memory = external_memory;
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});

  auto expect_rejected = [&](amdf_memory_import_info_t import_info,
                             amdf_status_code_t expected_code) {
    const amdf_external_memory_t original_external_memory = external_memory;
    const uint32_t prior_import_call_count = device.import_call_count;
    amdf_memory_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(amdf_memory_import(&device.base, &import_info,
                                                  &external_memory, &output)),
              expected_code);
    EXPECT_EQ(device.import_call_count, prior_import_call_count);
    EXPECT_EQ(output, sentinel);
    EXPECT_EQ(std::memcmp(&external_memory, &original_external_memory,
                          sizeof(external_memory)),
              0);
    EXPECT_EQ(release_state.count, 0u);
  };

  amdf_memory_import_info_t import_info = supported_import_info;
  import_info.memory_profile_ordinal = 1;
  expect_rejected(import_info, AMDF_STATUS_CODE_OUT_OF_RANGE);

  import_info = supported_import_info;
  device.profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_IMPORT;
  device.profile.import = {};
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  import_info.device_access |= AMDF_MEMORY_ACCESS_EXECUTE;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  import_info = supported_import_info;
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT;
  device.profile.external_memory_support_count = 2;
  device.profile.external_memory_support[1].type =
      AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  device.profile.external_memory_support[1].flags =
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT;
  device.profile.external_memory_support[1].byte_length_alignment = 1;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET;
  device.profile.external_memory_support[0].source_offset_alignment = 0;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  external_memory.source_byte_offset = 1;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.external_memory_support[0].source_offset_alignment = 1;
  external_memory.source_byte_offset = 1;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  external_memory = supported_external_memory;
  device.profile.external_memory_support[0].maximum_byte_length = 4096;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  external_memory.byte_length = 4097;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  external_memory = supported_external_memory;
  external_memory.type = AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD;
  external_memory.source_byte_offset = 0;
  external_memory.byte_length = 4096;
  external_memory.provenance.words[0] = 7;
  external_memory.provenance.words[1] = 11;
  device.profile.external_memory_support[0].type =
      AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD;
  device.profile.external_memory_support[0].provenance.words[0] = 7;
  device.profile.external_memory_support[0].provenance.words[1] = 13;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  external_memory = supported_external_memory;
  external_memory.provenance.words[0] = 1;
  expect_rejected(import_info, AMDF_STATUS_CODE_INVALID_ARGUMENT);

  external_memory = supported_external_memory;
  amdf_external_memory_release(&external_memory);
  EXPECT_EQ(release_state.count, 1u);
}

TEST(MemoryExternalTest, RawAddressImportAdoptsReleaseUntilTeardown) {
  FakeDevice device;
  InitializeFakeDevice(53, &device);
  device.adopt_external_memory = true;
  device.profile.external_memory_support[0].type =
      AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  device.profile.external_memory_support[0].flags =
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT;
  device.profile.external_memory_support[0].source_offset_alignment = 0;
  device.profile.external_memory_support[0].byte_length_alignment = 1;
  ReleaseState release_state = {};
  alignas(4096) std::array<uint8_t, 4096> host_storage = {};
  amdf_external_memory_t external_memory = {};
  external_memory.type = AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  external_memory.payload.host_pointer = host_storage.data();
  external_memory.byte_length = sizeof(host_storage);
  external_memory.physical_backing_id = device.backing_id;
  external_memory.release = RecordRelease;
  external_memory.release_user_data = &release_state;

  amdf_memory_t* memory = nullptr;
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo();
  ASSERT_EQ(
      amdf_memory_import(&device.base, &import_info, &external_memory, &memory),
      AMDF_STATUS_OK);
  EXPECT_EQ(release_state.count, 0u);
  const amdf_external_memory_t empty = {};
  EXPECT_EQ(std::memcmp(&external_memory, &empty, sizeof(external_memory)), 0);

  auto* native_memory = static_cast<FakeMemory*>(memory->native);
  const amdf_status_t release_failure =
      amdf_make_api_status(AMDF_STATUS_CODE_BUSY);
  native_memory->destroy_status = release_failure;
  EXPECT_EQ(amdf_memory_destroy(memory), release_failure);
  EXPECT_EQ(release_state.count, 0u);
  amdf_memory_info_t memory_info = {};
  memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  memory_info.structure_size = sizeof(memory_info);
  ASSERT_EQ(amdf_memory_query_info(memory, &memory_info), AMDF_STATUS_OK);
  EXPECT_EQ(memory_info.byte_length, sizeof(host_storage));
  native_memory->destroy_status = AMDF_STATUS_OK;

  ASSERT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
  EXPECT_EQ(release_state.count, 1u);
  EXPECT_EQ(release_state.type, AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER);
  EXPECT_EQ(release_state.payload.host_pointer, host_storage.data());
}

TEST(MemoryExternalTest, FailedCreateAndMapPreserveCallerStorage) {
  FakeDevice device;
  InitializeFakeDevice(59, &device);
  const amdf_status_t failure_status =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  device.create_status = failure_status;
  auto* const memory_sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* memory = memory_sentinel;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  EXPECT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            failure_status);
  EXPECT_EQ(memory, memory_sentinel);

  device.create_status = AMDF_STATUS_OK;
  ASSERT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            AMDF_STATUS_OK);
  auto* fake_memory = static_cast<FakeMemory*>(memory->native);
  fake_memory->map_status = failure_status;
  amdf_memory_map_info_t map_info = {};
  map_info.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
  map_info.structure_size = sizeof(map_info);
  map_info.byte_length = 4096;
  map_info.flags = AMDF_MEMORY_MAP_FLAG_READ;
  auto* const mapping_sentinel =
      reinterpret_cast<amdf_host_mapping_t*>(uintptr_t{1});
  amdf_host_mapping_t* mapping = mapping_sentinel;
  EXPECT_EQ(amdf_memory_map(memory, &map_info, &mapping), failure_status);
  EXPECT_EQ(mapping, mapping_sentinel);

  ASSERT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST(MemoryExternalTest, CreateProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(60, &device);
  const amdf_memory_profile_t supported_profile = device.profile;
  const amdf_memory_create_info_t supported_create_info =
      MakeMemoryCreateInfo();
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});

  auto expect_rejected = [&](amdf_memory_create_info_t create_info,
                             amdf_status_code_t expected_code) {
    const uint32_t prior_create_call_count = device.create_call_count;
    amdf_memory_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(
                  amdf_memory_create(&device.base, &create_info, &output)),
              expected_code);
    EXPECT_EQ(device.create_call_count, prior_create_call_count);
    EXPECT_EQ(output, sentinel);
  };

  amdf_memory_create_info_t create_info = supported_create_info;
  create_info.memory_profile_ordinal = 1;
  expect_rejected(create_info, AMDF_STATUS_CODE_OUT_OF_RANGE);

  create_info = supported_create_info;
  device.profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
  device.profile.allocation = {};
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  create_info.device_access |= AMDF_MEMORY_ACCESS_EXECUTE;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  create_info = supported_create_info;
  create_info.required_flags |= AMDF_MEMORY_FLAG_HOST_COHERENT;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.allocation.maximum_byte_length = 2048;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.allocation.byte_length_granularity = 8192;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  create_info.minimum_alignment = 8192;
  device.profile.allocation.maximum_alignment = 4096;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  create_info = supported_create_info;
  create_info.registered_host_pointer = &create_info;
  expect_rejected(create_info, AMDF_STATUS_CODE_INVALID_ARGUMENT);

  create_info = supported_create_info;
  create_info.device_access |= UINT32_C(1) << 31;
  expect_rejected(create_info, AMDF_STATUS_CODE_INVALID_ARGUMENT);
}

TEST(MemoryExternalTest, MapProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(62, &device);
  const amdf_memory_profile_t supported_profile = device.profile;
  amdf_memory_t* memory = nullptr;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  ASSERT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            AMDF_STATUS_OK);
  auto* fake_memory = static_cast<FakeMemory*>(memory->native);
  const amdf_memory_map_info_t supported_map_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO,
      .structure_size = sizeof(amdf_memory_map_info_t),
      .byte_length = create_info.byte_length,
      .flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
  };
  auto* const sentinel = reinterpret_cast<amdf_host_mapping_t*>(uintptr_t{1});

  auto expect_rejected = [&](amdf_memory_map_info_t map_info,
                             amdf_status_code_t expected_code) {
    const uint32_t prior_map_call_count = fake_memory->map_call_count;
    amdf_host_mapping_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(amdf_memory_map(memory, &map_info, &output)),
              expected_code);
    EXPECT_EQ(fake_memory->map_call_count, prior_map_call_count);
    EXPECT_EQ(output, sentinel);
  };

  amdf_memory_map_info_t map_info = supported_map_info;
  device.profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
  device.profile.supported_flags &= ~AMDF_MEMORY_FLAG_HOST_VISIBLE;
  device.profile.host_mapping = {};
  expect_rejected(map_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.host_mapping.supported_access = AMDF_MEMORY_MAP_FLAG_READ;
  expect_rejected(map_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.host_mapping.maximum_byte_length = 2048;
  expect_rejected(map_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.host_mapping.byte_offset_granularity = 4096;
  map_info.byte_offset = 1;
  --map_info.byte_length;
  expect_rejected(map_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  memory->info.memory_profile_ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
  map_info = supported_map_info;
  expect_rejected(map_info, AMDF_STATUS_CODE_UNSUPPORTED);
  memory->info.memory_profile_ordinal = supported_profile.ordinal;

  ASSERT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST(MemoryExternalTest, FailedQueriesAndExportsPreserveCallerStorage) {
  FakeDevice source_device;
  FakeDevice destination_device;
  InitializeFakeDevice(61, &source_device);
  InitializeFakeDevice(71, &destination_device);

  source_device.profile_status =
      amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  amdf_memory_profile_t profile;
  std::memset(&profile, 0xA5, sizeof(profile));
  profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  profile.structure_size = sizeof(profile);
  profile.next = nullptr;
  const amdf_memory_profile_t original_profile = profile;
  EXPECT_EQ(amdf_device_query_memory_profile(&source_device.base, 0, &profile),
            source_device.profile_status);
  EXPECT_EQ(std::memcmp(&profile, &original_profile, sizeof(profile)), 0);
  source_device.profile_status = AMDF_STATUS_OK;

  amdf_memory_t* source_memory = nullptr;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  ASSERT_EQ(
      amdf_memory_create(&source_device.base, &create_info, &source_memory),
      AMDF_STATUS_OK);
  auto* fake_source_memory = static_cast<FakeMemory*>(source_memory->native);
  fake_source_memory->export_status =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  amdf_external_memory_t external_output;
  std::memset(&external_output, 0x5A, sizeof(external_output));
  const amdf_external_memory_t original_external_output = external_output;
  const amdf_memory_export_info_t export_info = MakeMemoryExportInfo();
  EXPECT_EQ(amdf_memory_export(source_memory, &export_info, &external_output),
            fake_source_memory->export_status);
  EXPECT_EQ(fake_source_memory->export_call_count, 1u);
  EXPECT_EQ(std::memcmp(&external_output, &original_external_output,
                        sizeof(external_output)),
            0);

  amdf_memory_t* destination_memory = nullptr;
  ASSERT_EQ(amdf_memory_create(&destination_device.base, &create_info,
                               &destination_memory),
            AMDF_STATUS_OK);
  const amdf_memory_site_t producer_site = MakeMemorySite(source_memory, 0);
  const amdf_memory_site_t different_consumer_site =
      MakeMemorySite(destination_memory, 0);
  amdf_memory_pair_info_t pair_info;
  std::memset(&pair_info, 0xC3, sizeof(pair_info));
  pair_info.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  pair_info.structure_size = sizeof(pair_info);
  pair_info.next = nullptr;
  const amdf_memory_pair_info_t original_pair_info = pair_info;
  destination_device.base.provider_instance =
      reinterpret_cast<amdf_instance_t*>(uintptr_t{2});
  EXPECT_EQ(amdf_status_code(amdf_memory_query_pair_info(
                &producer_site, &different_consumer_site, &pair_info)),
            AMDF_STATUS_CODE_FAILED_PRECONDITION);
  EXPECT_EQ(fake_source_memory->site_description_count, 0u);
  EXPECT_EQ(std::memcmp(&pair_info, &original_pair_info, sizeof(pair_info)), 0);
  destination_device.base.provider_instance =
      source_device.base.provider_instance;

  EXPECT_EQ(amdf_status_code(amdf_memory_query_pair_info(
                &producer_site, &different_consumer_site, &pair_info)),
            AMDF_STATUS_CODE_FAILED_PRECONDITION);
  EXPECT_EQ(fake_source_memory->site_description_count, 0u);
  EXPECT_EQ(std::memcmp(&pair_info, &original_pair_info, sizeof(pair_info)), 0);

  destination_memory->info.physical_backing_id =
      source_memory->info.physical_backing_id;
  const amdf_physical_memory_id_t source_backing_id =
      source_memory->info.physical_backing_id;
  source_memory->info.physical_backing_id = {};
  EXPECT_EQ(amdf_status_code(amdf_memory_query_pair_info(
                &producer_site, &different_consumer_site, &pair_info)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(fake_source_memory->site_description_count, 0u);
  EXPECT_EQ(std::memcmp(&pair_info, &original_pair_info, sizeof(pair_info)), 0);
  source_memory->info.physical_backing_id = source_backing_id;

  fake_source_memory->site_status =
      amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(amdf_memory_query_pair_info(&producer_site,
                                        &different_consumer_site, &pair_info),
            fake_source_memory->site_status);
  EXPECT_EQ(fake_source_memory->site_description_count, 1u);
  EXPECT_EQ(std::memcmp(&pair_info, &original_pair_info, sizeof(pair_info)), 0);

  fake_source_memory->site_status = AMDF_STATUS_OK;
  auto* fake_destination_memory =
      static_cast<FakeMemory*>(destination_memory->native);
  fake_destination_memory->site_status =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  EXPECT_EQ(amdf_memory_query_pair_info(&producer_site,
                                        &different_consumer_site, &pair_info),
            fake_destination_memory->site_status);
  EXPECT_EQ(fake_source_memory->site_description_count, 2u);
  EXPECT_EQ(fake_destination_memory->site_description_count, 1u);
  EXPECT_EQ(std::memcmp(&pair_info, &original_pair_info, sizeof(pair_info)), 0);

  ASSERT_EQ(amdf_memory_destroy(destination_memory), AMDF_STATUS_OK);
  ASSERT_EQ(amdf_memory_destroy(source_memory), AMDF_STATUS_OK);
}

TEST(MemoryExternalTest, ExportProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(73, &device);
  const amdf_memory_profile_t supported_profile = device.profile;
  amdf_memory_t* memory = nullptr;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo();
  ASSERT_EQ(amdf_memory_create(&device.base, &create_info, &memory),
            AMDF_STATUS_OK);
  memory->info.source_byte_offset = 4096;
  memory->info.native_allocation_byte_length = 8192;
  auto* fake_memory = static_cast<FakeMemory*>(memory->native);
  ReleaseState release_state = {};
  fake_memory->export_release_state = &release_state;
  const amdf_memory_export_info_t supported_export_info =
      MakeMemoryExportInfo();

  amdf_external_memory_t external_output;
  std::memset(&external_output, 0x6B, sizeof(external_output));
  const amdf_external_memory_t original_external_output = external_output;
  auto expect_rejected = [&](amdf_memory_export_info_t export_info,
                             amdf_status_code_t expected_code) {
    const uint32_t prior_export_call_count = fake_memory->export_call_count;
    EXPECT_EQ(amdf_status_code(
                  amdf_memory_export(memory, &export_info, &external_output)),
              expected_code);
    EXPECT_EQ(fake_memory->export_call_count, prior_export_call_count);
    EXPECT_EQ(std::memcmp(&external_output, &original_external_output,
                          sizeof(external_output)),
              0);
    EXPECT_EQ(release_state.count, 0u);
  };

  amdf_memory_export_info_t export_info = supported_export_info;
  memory->info.flags &= ~AMDF_MEMORY_FLAG_SHAREABLE;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);
  memory->info.flags |= AMDF_MEMORY_FLAG_SHAREABLE;

  memory->info.memory_profile_ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);
  memory->info.memory_profile_ordinal = 0;

  device.profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_EXPORT;
  device.profile.supported_flags &= ~AMDF_MEMORY_FLAG_SHAREABLE;
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT;
  device.profile.external_memory_support_count = 2;
  device.profile.external_memory_support[1].type =
      AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  device.profile.external_memory_support[1].flags =
      AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT;
  device.profile.external_memory_support[1].byte_length_alignment = 1;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.external_memory_support[0].type =
      AMDF_EXTERNAL_MEMORY_TYPE_HOST_POINTER;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET;
  device.profile.external_memory_support[0].source_offset_alignment = 0;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.profile.external_memory_support[0].maximum_byte_length = 2048;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  export_info.byte_length = 2048;
  expect_rejected(export_info, AMDF_STATUS_CODE_UNSUPPORTED);

  ASSERT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

}  // namespace
