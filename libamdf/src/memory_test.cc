// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "amdf/amdf.h"
#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"
#include "libamdf/src/child_tracker.h"
#include "libamdf/src/device.h"
#include "libamdf/src/host_mapping.h"
#include "libamdf/src/instance.h"

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
  amdf_memory_native_profile_t profile;
  // Caller-owned access request consumed synchronously by construction.
  amdf_memory_device_access_t request;
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
  // Observer of temporary transports exported during aggregate construction.
  ReleaseState export_release;
  // Optional caller-owned log of native release attempts in device order.
  std::vector<uint64_t>* release_order;
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
};

static amdf_status_t FakeMemoryExport(
    amdf_memory_t* base_memory, uint32_t access_ordinal,
    const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value) {
  (void)export_info;
  auto* memory =
      static_cast<FakeMemory*>(base_memory->accesses[access_ordinal].native);
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
    amdf_memory_t* base_memory, uint32_t access_ordinal,
    uint32_t queue_family_ordinal,
    amdf_memory_site_description_t* out_description) {
  auto* memory =
      static_cast<FakeMemory*>(base_memory->accesses[access_ordinal].native);
  ++memory->site_description_count;
  memory->last_queue_family_ordinal = queue_family_ordinal;
  if (!amdf_status_is_ok(memory->site_status)) {
    return memory->site_status;
  }
  *out_description = memory->site_description;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeMemoryMap(
    amdf_memory_t* memory, uint32_t access_ordinal,
    const amdf_host_mapping_capabilities_t* capabilities,
    const amdf_memory_map_info_t* map_info, amdf_host_mapping_t** out_mapping) {
  (void)capabilities;
  (void)map_info;
  (void)out_mapping;
  auto* fake_memory =
      static_cast<FakeMemory*>(memory->accesses[access_ordinal].native);
  ++fake_memory->map_call_count;
  if (!amdf_status_is_ok(fake_memory->map_status)) {
    return fake_memory->map_status;
  }
  return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
}

static amdf_status_t FakeMemoryDestroyNative(amdf_memory_t* base_memory,
                                             uint32_t access_ordinal) {
  auto* memory =
      static_cast<FakeMemory*>(base_memory->accesses[access_ordinal].native);
  ++memory->device->destroy_call_count;
  if (memory->device->release_order != nullptr) {
    memory->device->release_order->push_back(
        memory->device->backing_id.words[0]);
  }
  if (!amdf_status_is_ok(memory->destroy_status)) {
    return memory->destroy_status;
  }
  amdf_free(base_memory->host_allocator, memory);
  base_memory->accesses[access_ordinal].native = nullptr;
  return AMDF_STATUS_OK;
}

static void FakeMemoryAbandonNative(amdf_memory_t* memory,
                                    uint32_t access_ordinal) {
  ++static_cast<FakeMemory*>(memory->accesses[access_ordinal].native)
        ->device->abandon_call_count;
  amdf_free(memory->host_allocator, memory->accesses[access_ordinal].native);
  memory->accesses[access_ordinal].native = nullptr;
}

static const amdf_memory_vtable_t kFakeMemoryVtable = {
    .export_external = FakeMemoryExport,
    .describe_site = FakeMemoryDescribeSite,
    .map = FakeMemoryMap,
    .destroy_native = FakeMemoryDestroyNative,
    .abandon_native = FakeMemoryAbandonNative,
};

static amdf_status_t PrepareFakeMemory(
    amdf_memory_t* base_memory, uint32_t access_ordinal,
    amdf_memory_access_t device_access, uint32_t memory_profile_ordinal,
    uint64_t source_byte_offset, uint64_t byte_length,
    amdf_physical_memory_id_t backing_id, amdf_memory_info_t* out_info) {
  auto* device = reinterpret_cast<FakeDevice*>(
      base_memory->accesses[access_ordinal].device);
  const amdf_allocator_t host_allocator =
      amdf_device_host_allocator(&device->base);
  FakeMemory* memory = nullptr;
  const amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*memory), amdf_alignof(FakeMemory),
                  reinterpret_cast<void**>(&memory));
  if (!amdf_status_is_ok(status)) return status;
  base_memory->accesses[access_ordinal].native = memory;
  memory->device = device;
  memory->export_status = AMDF_STATUS_OK;
  memory->map_status = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  memory->site_status = AMDF_STATUS_OK;
  memory->destroy_status = device->destroy_status;
  memory->export_release_state = &device->export_release;
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
  out_info->type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  out_info->structure_size = sizeof(*out_info);
  out_info->memory_profile_ordinal = memory_profile_ordinal;
  out_info->memory_class = AMDF_MEMORY_CLASS_SYSTEM;
  base_memory->accesses[access_ordinal].info.access = device_access;
  base_memory->accesses[access_ordinal].info.atomic_operations_32 =
      device->profile.atomic_operations_32;
  base_memory->accesses[access_ordinal].info.atomic_operations_64 =
      device->profile.atomic_operations_64;
  base_memory->accesses[access_ordinal].info.address_domain_ordinal = 0;
  out_info->flags = device->profile.supported_flags & AMDF_MEMORY_BACKING_FLAGS;
  base_memory->accesses[access_ordinal].info.flags =
      device->profile.guaranteed_flags & AMDF_MEMORY_ACCESS_FLAGS;
  out_info->source_byte_offset = source_byte_offset;
  out_info->byte_length = byte_length;
  out_info->alignment = 4096;
  out_info->native_allocation_byte_length = source_byte_offset + byte_length;
  out_info->native_allocation_granularity = 4096;
  out_info->physical_backing_id = backing_id;
  std::memcpy(base_memory->accesses[access_ordinal].addresses,
              device->addresses.data(),
              sizeof(base_memory->accesses[access_ordinal].addresses));
  base_memory->accesses[access_ordinal].info.address_kinds =
      device->profile.address_kinds;
  base_memory->accesses[access_ordinal].info.reset_epoch = 1;
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeDeviceQueryMemoryProfile(
    amdf_device_t* base_device, uint32_t memory_profile_ordinal,
    amdf_memory_native_profile_t* out_profile) {
  auto* device = reinterpret_cast<FakeDevice*>(base_device);
  if (!amdf_status_is_ok(device->profile_status)) {
    return device->profile_status;
  }
  if (memory_profile_ordinal > 1) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  *out_profile = device->profile;
  out_profile->ordinal = memory_profile_ordinal;
  if (memory_profile_ordinal == 0) {
    out_profile->roles &= ~AMDF_MEMORY_PROFILE_ROLE_IMPORT;
    out_profile->import = {};
  } else {
    out_profile->roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
    out_profile->allocation = {};
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t FakeDeviceMemoryPrepare(
    amdf_memory_t* memory, uint32_t access_ordinal,
    const amdf_memory_native_profile_t* profile,
    const amdf_memory_native_create_info_t* create_info,
    amdf_memory_info_t* out_info) {
  auto* device =
      reinterpret_cast<FakeDevice*>(memory->accesses[access_ordinal].device);
  memory->accesses[access_ordinal].vtable = &kFakeMemoryVtable;
  ++device->create_call_count;
  const uint64_t granularity =
      profile->allocation.native_byte_length_granularity;
  const uint64_t native_length =
      (create_info->byte_length + granularity - 1) & ~(granularity - 1);
  const amdf_status_t status = PrepareFakeMemory(
      memory, access_ordinal, create_info->device_access, profile->ordinal, 0,
      native_length, device->backing_id, out_info);
  return amdf_status_is_ok(status) ? device->create_status : status;
}

static amdf_status_t FakeDeviceMemoryPrepareImport(
    amdf_memory_t* memory, uint32_t access_ordinal,
    const amdf_memory_native_profile_t* profile,
    const amdf_memory_native_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_memory_info_t* out_info) {
  auto* device =
      reinterpret_cast<FakeDevice*>(memory->accesses[access_ordinal].device);
  memory->accesses[access_ordinal].vtable = &kFakeMemoryVtable;
  ++device->import_call_count;
  if (device->import_failure_stage == ImportFailureStage::kBeforeAttachment) {
    return device->import_status;
  }

  const amdf_status_t status = PrepareFakeMemory(
      memory, access_ordinal, import_info->device_access, profile->ordinal,
      external_memory->source_byte_offset, external_memory->byte_length,
      external_memory->physical_backing_id, out_info);
  if (!amdf_status_is_ok(status)) return status;
  if (device->import_failure_stage == ImportFailureStage::kAfterAttachment) {
    return device->import_status;
  }
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

static void InitializeFakeDevice(uint64_t identity, amdf_instance_t* instance,
                                 FakeDevice* out_device) {
  std::memset(out_device, 0, sizeof(*out_device));
  out_device->base.host_allocator = instance->host_allocator;
  out_device->base.vtable = &kFakeDeviceVtable;
  out_device->base.provider_instance = instance;
  out_device->base.engine_kind = AMDF_ENGINE_KIND_GPU;
  amdf_child_tracker_initialize(&out_device->base.children);
  out_device->request.device = &out_device->base;
  out_device->request.requirements.access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  out_device->request.requirements.flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
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

static amdf_memory_create_info_t MakeMemoryCreateInfo(FakeDevice& device) {
  amdf_memory_create_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  info.structure_size = sizeof(info);
  info.memory_profile_ordinal = 0;
  info.access_count = 1;
  info.accesses = &device.request;
  info.required_flags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_SHAREABLE;
  info.byte_length = 4096;
  info.minimum_alignment = 4096;
  return info;
}

static amdf_memory_import_info_t MakeMemoryImportInfo(FakeDevice& device) {
  amdf_memory_import_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_MEMORY_IMPORT_INFO;
  info.structure_size = sizeof(info);
  info.memory_profile_ordinal = 2;
  info.access_count = 1;
  info.accesses = &device.request;
  info.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
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
  site.value.device.memory = memory;
  site.value.device.queue_family_ordinal = queue_family_ordinal;
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

// Only native device operations are substituted. Construction, selection and
// teardown use the production common owner and a real instance representation.
class MemoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    instance_.host_allocator = amdf_allocator_system();
    instance_.system_memory_scope.kind = AMDF_MEMORY_SCOPE_KIND_SYSTEM;
    instance_.system_memory_scope.owner.instance = &instance_;
  }

  // Common lifetime owner; these tests do not require a platform connection.
  amdf_instance_t instance_ = {};
};

using MemoryConstructionTest = MemoryTest;
using MemoryAddressTest = MemoryTest;
using MemoryExternalTest = MemoryTest;

TEST_F(MemoryConstructionTest,
       NativeRoundingDoesNotExpandTheSharedLogicalRange) {
  FakeDevice devices[2];
  amdf_memory_device_access_t accesses[2];
  for (uint32_t i = 0; i < 2; ++i) {
    InitializeFakeDevice(i + 1, &instance_, &devices[i]);
    accesses[i] = devices[i].request;
    devices[i].profile.external_memory_support[0].byte_length_alignment = 1;
  }
  const uint64_t length = 4099;
  for (uint32_t count = 1; count <= 2; ++count) {
    amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(devices[0]);
    create_info.access_count = count;
    create_info.accesses = accesses;
    create_info.byte_length = length;
    amdf_memory_t* memory = nullptr;
    ASSERT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                                 &memory),
              AMDF_STATUS_OK);
    EXPECT_EQ(memory->info.byte_length, length);
    EXPECT_EQ(memory->info.native_allocation_byte_length, 8192u);
    amdf_memory_export_info_t export_info = MakeMemoryExportInfo();
    export_info.byte_offset = length;
    export_info.byte_length = 1;
    amdf_external_memory_t output = {};
    EXPECT_EQ(
        amdf_status_code(amdf_memory_export(memory, &export_info, &output)),
        AMDF_STATUS_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
  }
}

TEST_F(MemoryTest, HostSitesUseTheSelectedPeerAndPreserveNativeApiOperations) {
  FakeDevice devices[2];
  amdf_memory_device_access_t accesses[2];
  for (uint32_t i = 0; i < 2; ++i) {
    InitializeFakeDevice(i + 1, &instance_, &devices[i]);
    accesses[i] = devices[i].request;
  }
  devices[1].profile.guaranteed_flags |= AMDF_MEMORY_FLAG_HOST_COHERENT;
  devices[1].profile.supported_flags |= AMDF_MEMORY_FLAG_HOST_COHERENT;
  amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(devices[0]);
  create_info.access_count = 2;
  create_info.accesses = accesses;
  amdf_memory_t* memory = nullptr;
  ASSERT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      AMDF_STATUS_OK);

  // Supply native mapping facts to the production pair composer. Queries must
  // neither dereference host payload nor perform a cache operation.
  amdf_host_mapping_t mapping = {};
  mapping.memory = memory;
  mapping.info.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  mapping.info.cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
  mapping.info.flush.kind = AMDF_CACHE_TRANSITION_KIND_RANGE;
  mapping.info.flush.executor = AMDF_CACHE_TRANSITION_EXECUTOR_HOST_DIRECT;
  mapping.info.flush.host_operation = AMDF_HOST_CACHE_OPERATION_FLUSH;
  mapping.info.flush.host_instruction = AMDF_HOST_CACHE_INSTRUCTION_X86_CLFLUSH;
  mapping.info.flush.range_granularity = 64;
  mapping.info.invalidate = mapping.info.flush;
  mapping.info.invalidate.host_operation = AMDF_HOST_CACHE_OPERATION_INVALIDATE;
  amdf_memory_site_t host = {};
  host.type = AMDF_STRUCTURE_TYPE_MEMORY_SITE;
  host.structure_size = sizeof(host);
  host.kind = AMDF_MEMORY_SITE_KIND_HOST;
  host.value.host_mapping = &mapping;
  amdf_memory_pair_info_t pair = {};
  pair.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  pair.structure_size = sizeof(pair);
  for (uint32_t i = 0; i < 2; ++i) {
    amdf_memory_site_t device = MakeMemorySite(memory, 3);
    device.value.device.access_ordinal = i;
    ASSERT_EQ(amdf_memory_query_pair_info(&host, &device, &pair),
              AMDF_STATUS_OK);
    EXPECT_EQ(pair.release.kind, i == 0 ? AMDF_CACHE_TRANSITION_KIND_RANGE
                                        : AMDF_CACHE_TRANSITION_KIND_NONE);
    ASSERT_EQ(amdf_memory_query_pair_info(&device, &host, &pair),
              AMDF_STATUS_OK);
    EXPECT_EQ(pair.acquire.kind, i == 0 ? AMDF_CACHE_TRANSITION_KIND_RANGE
                                        : AMDF_CACHE_TRANSITION_KIND_NONE);
    EXPECT_EQ(pair.atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_NONE);
    EXPECT_EQ(pair.atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_NONE);
  }
  amdf_memory_site_t coherent = MakeMemorySite(memory, 3);
  coherent.value.device.access_ordinal = 1;
  mapping.info.flush.executor = AMDF_CACHE_TRANSITION_EXECUTOR_HOST_API;
  mapping.info.flush.host_instruction = AMDF_HOST_CACHE_INSTRUCTION_NONE;
  ASSERT_EQ(amdf_memory_query_pair_info(&host, &coherent, &pair),
            AMDF_STATUS_OK);
  EXPECT_EQ(
      std::memcmp(&pair.release, &mapping.info.flush, sizeof(pair.release)), 0);

  // A range-free WC fence remains necessary even with a coherent peer.
  mapping.info.cacheability = AMDF_HOST_CACHEABILITY_WRITE_COMBINED;
  mapping.info.flush = {};
  mapping.info.flush.kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL;
  mapping.info.flush.executor = AMDF_CACHE_TRANSITION_EXECUTOR_HOST_DIRECT;
  mapping.info.flush.host_operation = AMDF_HOST_CACHE_OPERATION_FLUSH;
  mapping.info.flush.host_fence_after = AMDF_HOST_CACHE_FENCE_X86_MFENCE;
  ASSERT_EQ(amdf_memory_query_pair_info(&host, &coherent, &pair),
            AMDF_STATUS_OK);
  EXPECT_EQ(
      std::memcmp(&pair.release, &mapping.info.flush, sizeof(pair.release)), 0);

  const amdf_memory_pair_info_t original = pair;
  mapping.info.flush.kind = AMDF_CACHE_TRANSITION_KIND_UNKNOWN;
  EXPECT_EQ(
      amdf_status_code(amdf_memory_query_pair_info(&host, &coherent, &pair)),
      AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(std::memcmp(&pair, &original, sizeof(pair)), 0);
  mapping.info.flags = AMDF_MEMORY_MAP_FLAG_READ;
  EXPECT_EQ(
      amdf_status_code(amdf_memory_query_pair_info(&host, &coherent, &pair)),
      AMDF_STATUS_CODE_UNSUPPORTED);
  for (uint32_t kind :
       std::array<uint32_t, 2>{AMDF_MEMORY_SITE_KIND_HOST, UINT32_MAX}) {
    host.kind = kind;
    host.value.host_mapping = nullptr;
    EXPECT_EQ(
        amdf_status_code(amdf_memory_query_pair_info(&host, &coherent, &pair)),
        AMDF_STATUS_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(std::memcmp(&pair, &original, sizeof(pair)), 0);
  }
  coherent.reserved = 1;
  EXPECT_EQ(amdf_status_code(
                amdf_memory_query_pair_info(&coherent, &coherent, &pair)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  coherent.reserved = 0;
  coherent.value.device.access_ordinal = 2;
  EXPECT_EQ(amdf_status_code(
                amdf_memory_query_pair_info(&coherent, &coherent, &pair)),
            AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(std::memcmp(&pair, &original, sizeof(pair)), 0);
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST_F(MemoryConstructionTest,
       PreservesCallerOrderWhenBackingComesFromLaterAccess) {
  std::array<FakeDevice, 3> devices;
  std::array<amdf_memory_device_access_t, 3> accesses;
  std::vector<uint64_t> release_order;
  for (size_t i = 0; i < devices.size(); ++i) {
    InitializeFakeDevice(i + 1, &instance_, &devices[i]);
    devices[i].addresses[AMDF_MEMORY_ADDRESS_GPU] = (i + 1) * 0x100000;
    devices[i].release_order = &release_order;
    accesses[i] = devices[i].request;
  }
  devices[0].profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
  accesses[0].requirements.access = AMDF_MEMORY_ACCESS_READ;
  amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(devices[1]);
  create_info.access_count = accesses.size();
  create_info.accesses = accesses.data();
  amdf_memory_t* memory = nullptr;
  ASSERT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      AMDF_STATUS_OK);
  EXPECT_EQ(devices[0].create_call_count, 0u);
  EXPECT_EQ(devices[1].create_call_count, 1u);
  EXPECT_EQ(devices[2].create_call_count, 0u);
  EXPECT_EQ(devices[0].import_call_count, 1u);
  EXPECT_EQ(devices[1].import_call_count, 0u);
  EXPECT_EQ(devices[2].import_call_count, 1u);
  EXPECT_EQ(devices[1].export_release.count, 1u);
  EXPECT_EQ(memory->backing_access_ordinal, 1u);
  EXPECT_EQ(memory->info.access_count, accesses.size());
  EXPECT_TRUE(amdf_physical_memory_id_is_equal(
      &memory->info.physical_backing_id, &devices[1].backing_id));
  for (uint32_t i = 0; i < accesses.size(); ++i) {
    devices[i].profile_status =
        amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
    amdf_memory_access_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_INFO;
    info.structure_size = sizeof(info);
    ASSERT_EQ(amdf_memory_query_access_info(memory, i, &info), AMDF_STATUS_OK);
    EXPECT_EQ(info.ordinal, i);
    EXPECT_EQ(info.access, accesses[i].requirements.access);
    uint64_t address = 0;
    ASSERT_EQ(
        amdf_memory_query_address(memory, i, AMDF_MEMORY_ADDRESS_GPU, &address),
        AMDF_STATUS_OK);
    EXPECT_EQ(address, devices[i].addresses[AMDF_MEMORY_ADDRESS_GPU]);
  }
  // The handle itself establishes shared backing; no physical-ID lookup is
  // needed.
  memory->info.physical_backing_id = {};
  amdf_memory_site_t producer = MakeMemorySite(memory, 3);
  producer.value.device.access_ordinal = 1;
  amdf_memory_site_t consumer = MakeMemorySite(memory, 5);
  amdf_memory_pair_info_t pair = {};
  pair.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  pair.structure_size = sizeof(pair);
  ASSERT_EQ(amdf_memory_query_pair_info(&producer, &consumer, &pair),
            AMDF_STATUS_OK);
  EXPECT_NE(pair.flags & AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE, 0u);
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
  EXPECT_EQ(release_order, (std::vector<uint64_t>{3, 1, 2}));
  EXPECT_EQ(devices[1].export_release.count, 1u);
}

TEST_F(MemoryConstructionTest,
       FailedAggregatePreservesBackingAfterConsumerReleaseError) {
  const amdf_status_t release_error =
      amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  for (ImportFailureStage stage : {ImportFailureStage::kBeforeAttachment,
                                   ImportFailureStage::kAfterAttachment}) {
    for (amdf_status_t destroy_status : {AMDF_STATUS_OK, release_error}) {
      AllocationState allocations;
      instance_.host_allocator = allocations.allocator();
      std::array<FakeDevice, 3> devices;
      std::array<amdf_memory_device_access_t, 3> accesses;
      std::vector<uint64_t> release_order;
      for (size_t i = 0; i < devices.size(); ++i) {
        InitializeFakeDevice(i + 1, &instance_, &devices[i]);
        devices[i].release_order = &release_order;
        accesses[i] = devices[i].request;
      }
      devices[0].profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
      devices[0].destroy_status = destroy_status;
      devices[2].import_failure_stage = stage;
      amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(devices[1]);
      create_info.access_count = accesses.size();
      create_info.accesses = accesses.data();
      auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
      amdf_memory_t* memory = sentinel;
      EXPECT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                                   &memory),
                amdf_status_is_ok(destroy_status) ? devices[2].import_status
                                                  : destroy_status);
      EXPECT_EQ(memory, sentinel);
      EXPECT_EQ(devices[1].export_release.count, 1u);
      std::vector<uint64_t> expected_order;
      if (stage == ImportFailureStage::kAfterAttachment)
        expected_order.push_back(3);
      expected_order.push_back(1);
      if (amdf_status_is_ok(destroy_status)) expected_order.push_back(2);
      EXPECT_EQ(release_order, expected_order);
      EXPECT_EQ(devices[0].abandon_call_count,
                amdf_status_is_ok(destroy_status) ? 0u : 1u);
      EXPECT_EQ(devices[1].abandon_call_count,
                amdf_status_is_ok(destroy_status) ? 0u : 1u);
      EXPECT_EQ(devices[2].abandon_call_count, 0u);
      EXPECT_EQ(allocations.live_count, 0u);
    }
  }
}

TEST_F(MemoryConstructionTest,
       RejectsDuplicateAndForeignLiveConsumersBeforeNativeWork) {
  FakeDevice device;
  InitializeFakeDevice(1, &instance_, &device);
  std::array<amdf_memory_device_access_t, 2> accesses = {device.request,
                                                         device.request};
  amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
  create_info.access_count = accesses.size();
  create_info.accesses = accesses.data();
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* memory = sentinel;
  EXPECT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT));
  EXPECT_EQ(memory, sentinel);
  amdf_instance_t other_instance = {};
  device.base.provider_instance = &other_instance;
  create_info.access_count = 1;
  EXPECT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT));
  EXPECT_EQ(memory, sentinel);
  EXPECT_EQ(device.create_call_count, 0u);
  EXPECT_EQ(device.import_call_count, 0u);
}

TEST_F(MemoryConstructionTest, AllocationFailureHasNoNativeReleaseObligation) {
  for (uint32_t failure_ordinal : {0u, 1u, 2u}) {
    AllocationState allocations;
    allocations.failure_ordinal = failure_ordinal;
    FakeDevice device;
    InitializeFakeDevice(7, &instance_, &device);
    device.base.host_allocator = allocations.allocator();
    instance_.host_allocator = allocations.allocator();
    const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
    auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
    amdf_memory_t* memory = sentinel;
    EXPECT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                                 &memory),
              amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED));
    EXPECT_EQ(memory, sentinel);
    EXPECT_EQ(device.create_call_count, failure_ordinal == 2 ? 1u : 0u);
    EXPECT_EQ(device.destroy_call_count, 0u);
    EXPECT_EQ(device.abandon_call_count, 0u);
    EXPECT_EQ(allocations.live_count, 0u);
  }
}

TEST_F(MemoryConstructionTest,
       AggregateAllocationFailuresPreserveTheInputMove) {
  for (amdf_memory_profile_roles_t role :
       {AMDF_MEMORY_PROFILE_ROLE_CREATE, AMDF_MEMORY_PROFILE_ROLE_IMPORT}) {
    // Plan, common owner, then three independently prepared native accesses.
    for (uint32_t failure_ordinal = 0; failure_ordinal <= 5;
         ++failure_ordinal) {
      SCOPED_TRACE(::testing::Message()
                   << "role=" << role << " allocation=" << failure_ordinal);
      AllocationState allocations;
      allocations.failure_ordinal = failure_ordinal;
      instance_.host_allocator = allocations.allocator();
      std::array<FakeDevice, 3> devices;
      std::array<amdf_memory_device_access_t, 3> accesses;
      for (uint32_t i = 0; i < devices.size(); ++i) {
        InitializeFakeDevice(i + 1, &instance_, &devices[i]);
        accesses[i] = devices[i].request;
      }
      devices[0].profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
      ReleaseState release = {};
      amdf_external_memory_t external = {};
      external.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
      external.payload.file_descriptor = 83;
      external.byte_length = 4096;
      external.physical_backing_id = devices[0].backing_id;
      external.release = RecordRelease;
      external.release_user_data = &release;
      const amdf_external_memory_t original = external;
      auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
      amdf_memory_t* memory = sentinel;
      amdf_status_t status;
      if (role == AMDF_MEMORY_PROFILE_ROLE_CREATE) {
        amdf_memory_create_info_t create_info =
            MakeMemoryCreateInfo(devices[1]);
        create_info.access_count = accesses.size();
        create_info.accesses = accesses.data();
        status = amdf_memory_create(&instance_.system_memory_scope,
                                    &create_info, &memory);
        EXPECT_EQ(devices[1].export_release.count,
                  failure_ordinal >= 3 ? 1u : 0u);
      } else {
        amdf_memory_import_info_t import_info =
            MakeMemoryImportInfo(devices[0]);
        import_info.access_count = accesses.size();
        import_info.accesses = accesses.data();
        status = amdf_memory_import(&instance_.system_memory_scope,
                                    &import_info, &external, &memory);
        EXPECT_EQ(release.count, failure_ordinal == 5 ? 1u : 0u);
      }
      if (failure_ordinal == 5) {
        ASSERT_EQ(status, AMDF_STATUS_OK);
        EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
      } else {
        EXPECT_EQ(amdf_status_code(status),
                  AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
        EXPECT_EQ(memory, sentinel);
        EXPECT_EQ(std::memcmp(&external, &original, sizeof(external)), 0);
      }
      uint32_t release_count = 0;
      for (const auto& device : devices) {
        release_count += device.destroy_call_count;
        EXPECT_EQ(device.abandon_call_count, 0u);
      }
      EXPECT_EQ(release_count, failure_ordinal > 2 ? failure_ordinal - 2 : 0u);
      EXPECT_EQ(allocations.live_count, 0u);
      amdf_external_memory_release(&external);
      EXPECT_EQ(release.count, 1u);
    }
  }
}

TEST_F(MemoryConstructionTest, FailedPrepareDiscardsUnpublishedNativeMetadata) {
  const amdf_status_t release_error =
      amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  for (amdf_status_t destroy_status : {AMDF_STATUS_OK, release_error}) {
    AllocationState allocations;
    FakeDevice device;
    InitializeFakeDevice(9, &instance_, &device);
    device.base.host_allocator = allocations.allocator();
    instance_.host_allocator = allocations.allocator();
    device.create_status =
        amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
    device.destroy_status = destroy_status;
    const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
    auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
    amdf_memory_t* memory = sentinel;
    EXPECT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                                 &memory),
              amdf_status_is_ok(destroy_status) ? device.create_status
                                                : destroy_status);
    EXPECT_EQ(memory, sentinel);
    EXPECT_EQ(device.destroy_call_count, 1u);
    EXPECT_EQ(device.abandon_call_count,
              amdf_status_is_ok(destroy_status) ? 0u : 1u);
    EXPECT_EQ(allocations.live_count, 0u);
  }
}

TEST_F(MemoryConstructionTest, FailedImportPreservesInputOnReleaseError) {
  AllocationState allocations;
  FakeDevice device;
  InitializeFakeDevice(13, &instance_, &device);
  device.base.host_allocator = allocations.allocator();
  instance_.host_allocator = allocations.allocator();
  device.import_failure_stage = ImportFailureStage::kAfterAttachment;
  device.destroy_status = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  ReleaseState release = {};
  amdf_external_memory_t external = {};
  external.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  external.byte_length = 4096;
  external.payload.file_descriptor = 91;
  external.physical_backing_id = device.backing_id;
  external.release = RecordRelease;
  external.release_user_data = &release;
  const amdf_external_memory_t original = external;
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo(device);
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* memory = sentinel;
  EXPECT_EQ(amdf_memory_import(&instance_.system_memory_scope, &import_info,
                               &external, &memory),
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

TEST_F(MemoryAddressTest, QueriesCachedAddressAndRejectsUnavailableConsumers) {
  FakeDevice device;
  InitializeFakeDevice(11, &instance_, &device);
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
  amdf_memory_t* memory = nullptr;
  ASSERT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      AMDF_STATUS_OK);
  amdf_memory_info_t memory_info = {};
  memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  memory_info.structure_size = sizeof(memory_info);
  ASSERT_EQ(amdf_memory_query_info(memory, &memory_info), AMDF_STATUS_OK);
  EXPECT_EQ(memory_info.access_count, 1u);
  EXPECT_EQ(memory_info.flags & AMDF_MEMORY_ACCESS_FLAGS, 0u);
  amdf_memory_access_info_t access_info = {};
  access_info.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_INFO;
  access_info.structure_size = sizeof(access_info);
  ASSERT_EQ(amdf_memory_query_access_info(memory, 0, &access_info),
            AMDF_STATUS_OK);
  EXPECT_EQ(access_info.ordinal, 0u);
  EXPECT_EQ(access_info.access, device.request.requirements.access);
  EXPECT_EQ(access_info.flags, AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
  EXPECT_EQ(access_info.address_kinds, UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU);
  EXPECT_EQ(access_info.reset_epoch, 1u);
  const amdf_memory_access_info_t original_access_info = access_info;
  EXPECT_EQ(
      amdf_status_code(amdf_memory_query_access_info(memory, 1, &access_info)),
      AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(
      std::memcmp(&access_info, &original_access_info, sizeof(access_info)), 0);
  EXPECT_EQ(
      amdf_status_code(amdf_memory_query_access_info(nullptr, 0, &access_info)),
      AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(
      std::memcmp(&access_info, &original_access_info, sizeof(access_info)), 0);
  EXPECT_EQ(amdf_status_code(amdf_memory_query_access_info(memory, 0, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  access_info.type = AMDF_STRUCTURE_TYPE_NONE;
  const amdf_memory_access_info_t invalid_access_info = access_info;
  EXPECT_EQ(
      amdf_status_code(amdf_memory_query_access_info(memory, 0, &access_info)),
      AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(
      std::memcmp(&access_info, &invalid_access_info, sizeof(access_info)), 0);
  uint64_t address = 0;
  EXPECT_EQ(
      amdf_memory_query_address(memory, 0, AMDF_MEMORY_ADDRESS_GPU, &address),
      AMDF_STATUS_OK);
  EXPECT_EQ(address, UINT64_C(0x100000));
  EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                memory, 1, AMDF_MEMORY_ADDRESS_GPU, &address)),
            AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(address, UINT64_C(0x100000));
  for (amdf_memory_address_kind_t kind :
       {AMDF_MEMORY_ADDRESS_XDNA_DMA, AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE}) {
    EXPECT_EQ(
        amdf_status_code(amdf_memory_query_address(memory, 0, kind, &address)),
        AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(address, UINT64_C(0x100000));
  }
  EXPECT_EQ(amdf_status_code(
                amdf_memory_query_address(memory, 0, UINT32_MAX, &address)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(address, UINT64_C(0x100000));
  EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                nullptr, 0, AMDF_MEMORY_ADDRESS_GPU, &address)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(address, UINT64_C(0x100000));
  EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                memory, 0, AMDF_MEMORY_ADDRESS_GPU, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  const auto* native =
      static_cast<const FakeMemory*>(memory->accesses[0].native);
  EXPECT_EQ(native->map_call_count, 0u);
  EXPECT_EQ(native->export_call_count, 0u);
  EXPECT_EQ(device.create_call_count, 1u);
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST_F(MemoryAddressTest, KeepsDistinctConsumerAddressesIncludingZero) {
  FakeDevice device;
  InitializeFakeDevice(12, &instance_, &device);
  device.profile.address_kinds =
      (UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_DMA) |
      (UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE);
  device.addresses[AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE] = 0;
  device.addresses[AMDF_MEMORY_ADDRESS_XDNA_DMA] = UINT64_C(0x80000000);
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
  amdf_memory_t* memory = nullptr;
  ASSERT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      AMDF_STATUS_OK);
  // A published object cannot consult the provider again for cached addresses.
  device.profile_status = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  for (int iteration = 0; iteration < 2; ++iteration) {
    uint64_t address = UINT64_MAX;
    EXPECT_EQ(amdf_memory_query_address(
                  memory, 0, AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE, &address),
              AMDF_STATUS_OK);
    EXPECT_EQ(address, 0u);
    EXPECT_EQ(amdf_memory_query_address(memory, 0, AMDF_MEMORY_ADDRESS_XDNA_DMA,
                                        &address),
              AMDF_STATUS_OK);
    EXPECT_EQ(address, UINT64_C(0x80000000));
    EXPECT_EQ(amdf_status_code(amdf_memory_query_address(
                  memory, 0, AMDF_MEMORY_ADDRESS_GPU, &address)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(address, UINT64_C(0x80000000));
  }
  const auto* native =
      static_cast<const FakeMemory*>(memory->accesses[0].native);
  EXPECT_EQ(native->map_call_count, 0u);
  EXPECT_EQ(native->export_call_count, 0u);
  EXPECT_EQ(device.create_call_count, 1u);
  EXPECT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
}

TEST_F(MemoryExternalTest, CompletesProfileExportImportPairAndReverseTeardown) {
  FakeDevice source_device;
  FakeDevice destination_device;
  InitializeFakeDevice(11, &instance_, &source_device);
  InitializeFakeDevice(29, &instance_, &destination_device);

  amdf_memory_t* source_memory = nullptr;
  const amdf_memory_create_info_t create_info =
      MakeMemoryCreateInfo(source_device);
  ASSERT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                               &source_memory),
            AMDF_STATUS_OK);
  ReleaseState transport_release = {};
  static_cast<FakeMemory*>(source_memory->accesses[0].native)
      ->export_release_state = &transport_release;

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
  const amdf_memory_import_info_t import_info =
      MakeMemoryImportInfo(destination_device);
  ASSERT_EQ(amdf_memory_import(&instance_.system_memory_scope, &import_info,
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
  EXPECT_EQ(destination_info.memory_profile_ordinal,
            import_info.memory_profile_ordinal);
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
  EXPECT_EQ(static_cast<FakeMemory*>(source_memory->accesses[0].native)
                ->last_queue_family_ordinal,
            3u);
  EXPECT_EQ(static_cast<FakeMemory*>(destination_memory->accesses[0].native)
                ->last_queue_family_ordinal,
            5u);

  auto* destination_fake =
      static_cast<FakeMemory*>(destination_memory->accesses[0].native);
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

TEST_F(MemoryExternalTest, ImportFailureNeverConsumesInputOrPublishesOutput) {
  FakeDevice device;
  InitializeFakeDevice(41, &instance_, &device);
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo(device);
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
    EXPECT_EQ(amdf_memory_import(&instance_.system_memory_scope, &import_info,
                                 &external_memory, &output),
              device.import_status);
    EXPECT_EQ(output, sentinel);
    EXPECT_EQ(std::memcmp(&external_memory, &original_external_memory,
                          sizeof(external_memory)),
              0);
    EXPECT_EQ(release_state.count, 0u);
  }

  amdf_memory_import_info_t invalid_info = import_info;
  device.request.requirements.access |= UINT32_C(1) << 31;
  const uint32_t prior_import_call_count = device.import_call_count;
  amdf_memory_t* output = sentinel;
  EXPECT_EQ(amdf_status_code(amdf_memory_import(&instance_.system_memory_scope,
                                                &invalid_info, &external_memory,
                                                &output)),
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

TEST_F(MemoryExternalTest, ImportProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(47, &instance_, &device);
  const amdf_memory_native_profile_t supported_profile = device.profile;
  const amdf_memory_import_info_t supported_import_info =
      MakeMemoryImportInfo(device);
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
    EXPECT_EQ(amdf_status_code(
                  amdf_memory_import(&instance_.system_memory_scope,
                                     &import_info, &external_memory, &output)),
              expected_code);
    EXPECT_EQ(device.import_call_count, prior_import_call_count);
    EXPECT_EQ(output, sentinel);
    EXPECT_EQ(std::memcmp(&external_memory, &original_external_memory,
                          sizeof(external_memory)),
              0);
    EXPECT_EQ(release_state.count, 0u);
  };

  amdf_memory_import_info_t import_info = supported_import_info;
  import_info.memory_profile_ordinal = 3;
  expect_rejected(import_info, AMDF_STATUS_CODE_OUT_OF_RANGE);

  import_info = supported_import_info;
  device.profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_IMPORT;
  device.profile.import = {};
  device.profile.external_memory_support[0].flags &=
      ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.request.requirements.access |= AMDF_MEMORY_ACCESS_EXECUTE;
  expect_rejected(import_info, AMDF_STATUS_CODE_UNSUPPORTED);
  device.request.requirements.access &= ~AMDF_MEMORY_ACCESS_EXECUTE;

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

TEST_F(MemoryExternalTest, ImportedReferenceSurvivesFailedPublishedDestroy) {
  FakeDevice device;
  InitializeFakeDevice(53, &instance_, &device);
  ReleaseState release_state = {};
  amdf_external_memory_t external_memory = {};
  external_memory.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
  external_memory.payload.file_descriptor = 101;
  external_memory.byte_length = 4096;
  external_memory.physical_backing_id = device.backing_id;
  external_memory.release = RecordRelease;
  external_memory.release_user_data = &release_state;

  amdf_memory_t* memory = nullptr;
  const amdf_memory_import_info_t import_info = MakeMemoryImportInfo(device);
  ASSERT_EQ(amdf_memory_import(&instance_.system_memory_scope, &import_info,
                               &external_memory, &memory),
            AMDF_STATUS_OK);
  EXPECT_EQ(release_state.count, 1u);
  const amdf_external_memory_t empty = {};
  EXPECT_EQ(std::memcmp(&external_memory, &empty, sizeof(external_memory)), 0);

  auto* native_memory = static_cast<FakeMemory*>(memory->accesses[0].native);
  const amdf_status_t release_failure =
      amdf_make_api_status(AMDF_STATUS_CODE_BUSY);
  native_memory->destroy_status = release_failure;
  EXPECT_EQ(amdf_memory_destroy(memory), release_failure);
  EXPECT_EQ(release_state.count, 1u);
  amdf_memory_info_t memory_info = {};
  memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  memory_info.structure_size = sizeof(memory_info);
  ASSERT_EQ(amdf_memory_query_info(memory, &memory_info), AMDF_STATUS_OK);
  EXPECT_EQ(memory_info.byte_length, 4096u);
  native_memory->destroy_status = AMDF_STATUS_OK;

  ASSERT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
  EXPECT_EQ(release_state.count, 1u);
  EXPECT_EQ(release_state.type, AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);
  EXPECT_EQ(release_state.payload.file_descriptor, 101);
}

TEST_F(MemoryExternalTest, FailedCreateAndMapPreserveCallerStorage) {
  FakeDevice device;
  InitializeFakeDevice(59, &instance_, &device);
  const amdf_status_t failure_status =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  device.create_status = failure_status;
  auto* const memory_sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* memory = memory_sentinel;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
  EXPECT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      failure_status);
  EXPECT_EQ(memory, memory_sentinel);

  device.create_status = AMDF_STATUS_OK;
  ASSERT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      AMDF_STATUS_OK);
  auto* fake_memory = static_cast<FakeMemory*>(memory->accesses[0].native);
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

TEST_F(MemoryExternalTest, CreateProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(60, &instance_, &device);
  const amdf_memory_native_profile_t supported_profile = device.profile;
  const amdf_memory_create_info_t supported_create_info =
      MakeMemoryCreateInfo(device);
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});

  auto expect_rejected = [&](amdf_memory_create_info_t create_info,
                             amdf_status_code_t expected_code) {
    const uint32_t prior_create_call_count = device.create_call_count;
    amdf_memory_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(amdf_memory_create(
                  &instance_.system_memory_scope, &create_info, &output)),
              expected_code);
    EXPECT_EQ(device.create_call_count, prior_create_call_count);
    EXPECT_EQ(output, sentinel);
  };

  amdf_memory_create_info_t create_info = supported_create_info;
  create_info.memory_profile_ordinal = 3;
  expect_rejected(create_info, AMDF_STATUS_CODE_OUT_OF_RANGE);

  create_info = supported_create_info;
  device.profile.roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
  device.profile.allocation = {};
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);

  device.profile = supported_profile;
  device.request.requirements.access |= AMDF_MEMORY_ACCESS_EXECUTE;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);
  device.request.requirements.access &= ~AMDF_MEMORY_ACCESS_EXECUTE;

  device.profile = supported_profile;
  create_info = supported_create_info;
  device.request.requirements.flags |= AMDF_MEMORY_FLAG_HOST_COHERENT;
  expect_rejected(create_info, AMDF_STATUS_CODE_UNSUPPORTED);
  device.request.requirements.flags &= ~AMDF_MEMORY_FLAG_HOST_COHERENT;

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
  device.request.requirements.access |= UINT32_C(1) << 31;
  expect_rejected(create_info, AMDF_STATUS_CODE_INVALID_ARGUMENT);
}

TEST_F(MemoryExternalTest, MapProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(62, &instance_, &device);
  const amdf_memory_native_profile_t supported_profile = device.profile;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
  const amdf_memory_map_info_t supported_map_info = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO,
      .structure_size = sizeof(amdf_memory_map_info_t),
      .byte_length = create_info.byte_length,
      .flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
  };
  auto* const sentinel = reinterpret_cast<amdf_host_mapping_t*>(uintptr_t{1});

  auto expect_rejected = [&](amdf_memory_map_info_t map_info,
                             amdf_status_code_t expected_code) {
    amdf_memory_create_info_t selected_create_info = create_info;
    selected_create_info.required_flags &= device.profile.supported_flags;
    amdf_memory_t* memory = nullptr;
    ASSERT_EQ(amdf_memory_create(&instance_.system_memory_scope,
                                 &selected_create_info, &memory),
              AMDF_STATUS_OK);
    auto* fake_memory = static_cast<FakeMemory*>(memory->accesses[0].native);
    // Mapping consumes immutable construction facts, not another device query.
    device.profile_status = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
    amdf_host_mapping_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(amdf_memory_map(memory, &map_info, &output)),
              expected_code);
    EXPECT_EQ(fake_memory->map_call_count, 0u);
    EXPECT_EQ(output, sentinel);
    ASSERT_EQ(amdf_memory_destroy(memory), AMDF_STATUS_OK);
    device.profile_status = AMDF_STATUS_OK;
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
}

TEST_F(MemoryExternalTest, FailedQueriesAndExportsPreserveCallerStorage) {
  FakeDevice source_device;
  FakeDevice destination_device;
  InitializeFakeDevice(61, &instance_, &source_device);
  InitializeFakeDevice(71, &instance_, &destination_device);

  source_device.profile_status =
      amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  const amdf_memory_create_info_t create_info =
      MakeMemoryCreateInfo(source_device);
  auto* const sentinel = reinterpret_cast<amdf_memory_t*>(uintptr_t{1});
  amdf_memory_t* source_memory = sentinel;
  EXPECT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                               &source_memory),
            source_device.profile_status);
  EXPECT_EQ(source_memory, sentinel);
  EXPECT_EQ(source_device.create_call_count, 0u);
  source_device.profile_status = AMDF_STATUS_OK;

  ASSERT_EQ(amdf_memory_create(&instance_.system_memory_scope, &create_info,
                               &source_memory),
            AMDF_STATUS_OK);
  auto* fake_source_memory =
      static_cast<FakeMemory*>(source_memory->accesses[0].native);
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
  const amdf_memory_create_info_t destination_create_info =
      MakeMemoryCreateInfo(destination_device);
  ASSERT_EQ(amdf_memory_create(&instance_.system_memory_scope,
                               &destination_create_info, &destination_memory),
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
  amdf_instance_t other_instance = {};
  destination_device.base.provider_instance = &other_instance;
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
      static_cast<FakeMemory*>(destination_memory->accesses[0].native);
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

TEST_F(MemoryExternalTest, ExportProfileRejectionPrecedesLeafMutation) {
  FakeDevice device;
  InitializeFakeDevice(73, &instance_, &device);
  const amdf_memory_native_profile_t supported_profile = device.profile;
  amdf_memory_t* memory = nullptr;
  const amdf_memory_create_info_t create_info = MakeMemoryCreateInfo(device);
  ASSERT_EQ(
      amdf_memory_create(&instance_.system_memory_scope, &create_info, &memory),
      AMDF_STATUS_OK);
  memory->info.source_byte_offset = 4096;
  memory->info.native_allocation_byte_length = 8192;
  auto* fake_memory = static_cast<FakeMemory*>(memory->accesses[0].native);
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
