// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/memory.h"

#include <emmintrin.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/kfd_ioctl.h>
#include <sys/ioctl.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/gpu/umd/kfd/buffer.h"
#include "libamdf/src/gpu/umd/kfd/device.h"
#include "libamdf/src/platform/linux/dma_buf.h"
#include "libamdf/src/platform/linux/file.h"
#include "libamdf/src/platform/linux/host_cache.h"

// One backing handle and GPU attachment, including incomplete teardown state.
struct amdf_gpu_umd_memory_t {
  // Device borrowed while memory metadata is live.
  amdf_gpu_umd_device_t* device;
  // Native allocation and CPU/GPU mapping, including partial preparation.
  amdf_gpu_kfd_buffer_t* buffer;
  // Page-covered native backing length in bytes.
  size_t byte_length;
  // Canonical DMA-BUF identity for shareable owned GTT.
  amdf_physical_memory_id_t physical_backing_id;
  // Borrowed caller pages or persistent mapping into the owned reservation.
  void* host_pointer;
  // Cache behavior of the host view.
  amdf_host_cacheability_t cacheability;
};

// Native allocation plan derived from one selected memory profile.
typedef struct amdf_gpu_kfd_memory_plan_t {
  // Properties established by the selected allocation class and native flags.
  amdf_memory_flags_t flags;
  // Native KFD allocation flags.
  uint32_t native_flags;
  // Page-covered native backing length.
  size_t byte_length;
  // Page-aligned GPU VA base alignment, including any stronger caller request.
  size_t alignment;
  // Offset of the requested range within registered caller pages.
  size_t host_byte_offset;
} amdf_gpu_kfd_memory_plan_t;

static void amdf_gpu_kfd_memory_plan(
    const amdf_gpu_umd_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info,
    amdf_gpu_kfd_memory_plan_t* plan) {
  plan->flags = profile->guaranteed_flags | create_info->required_flags;
  // KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE uses a signed shift into bit 31 in the
  // UAPI header. Construct that bit with an unsigned operand for defined C.
  plan->native_flags =
      (create_info->device_access & AMDF_MEMORY_ACCESS_WRITE) != 0
          ? UINT32_C(1) << 31
          : 0;
  if (profile->memory_class == AMDF_MEMORY_CLASS_LOCAL) {
    plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_VRAM;
    if ((plan->flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0) {
      plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC;
    }
  } else {
    plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
    if (profile->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST) {
      const uintptr_t pointer = (uintptr_t)create_info->registered_host_pointer;
      plan->host_byte_offset = pointer & (device->page_size - 1);
      plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_USERPTR;
    } else {
      plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_GTT;
    }
  }
  if ((create_info->device_access & AMDF_MEMORY_ACCESS_EXECUTE) != 0) {
    plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
  }
  const size_t padding = plan->host_byte_offset + device->page_size - 1;
  plan->byte_length =
      (create_info->byte_length + padding) & ~(device->page_size - 1);
  plan->alignment = create_info->minimum_alignment > device->page_size
                        ? (size_t)create_info->minimum_alignment
                        : device->page_size;
}

amdf_status_t amdf_gpu_umd_memory_destroy(amdf_gpu_umd_memory_t* memory) {
  if (memory->buffer != NULL) {
    const amdf_status_t status = amdf_gpu_kfd_buffer_destroy(memory->buffer);
    if (!amdf_status_is_ok(status)) return status;
    memory->buffer = NULL;
  }
  amdf_free(memory->device->host_allocator, memory);
  return AMDF_STATUS_OK;
}

void amdf_gpu_umd_memory_abandon(amdf_gpu_umd_memory_t* memory) {
  if (memory->buffer != NULL) {
    amdf_gpu_kfd_buffer_abandon(memory->buffer);
  }
  amdf_free(memory->device->host_allocator, memory);
}

static amdf_status_t amdf_gpu_kfd_memory_open_dma_buf(
    amdf_gpu_umd_memory_t* memory, int* out_descriptor,
    amdf_linux_dma_buf_info_t* out_info) {
  struct kfd_ioctl_export_dmabuf_args export_args = {
      .handle = amdf_gpu_kfd_buffer_handle(memory->buffer),
      .flags = O_CLOEXEC,
  };
  if (ioctl(memory->device->descriptor, AMDKFD_IOC_EXPORT_DMABUF,
            &export_args) != 0) {
    return amdf_linux_error(errno);
  }
  amdf_assert(export_args.dmabuf_fd <= INT_MAX &&
              "successful DMA-BUF export must return a native int descriptor");
  int descriptor = (int)export_args.dmabuf_fd;
  amdf_linux_dma_buf_info_t info;
  amdf_status_t status = amdf_linux_dma_buf_query(descriptor, &info);
  if (!amdf_status_is_ok(status)) {
    const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
    if (!amdf_status_is_ok(close_status)) status = close_status;
    return status;
  }
  *out_descriptor = descriptor;
  *out_info = info;
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_gpu_kfd_memory_query_dma_buf(
    amdf_gpu_umd_memory_t* memory, amdf_linux_dma_buf_info_t* out_info) {
  int descriptor = -1;
  amdf_linux_dma_buf_info_t info;
  amdf_status_t status =
      amdf_gpu_kfd_memory_open_dma_buf(memory, &descriptor, &info);
  if (amdf_status_is_ok(status) && info.byte_length != memory->byte_length) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
  if (!amdf_status_is_ok(close_status)) status = close_status;
  if (amdf_status_is_ok(status)) *out_info = info;
  return status;
}

static uint64_t amdf_gpu_kfd_maximum_byte_length(
    const amdf_gpu_umd_device_t* device) {
  const uint64_t virtual_address_span = device->topology.virtual_address.end -
                                        device->topology.virtual_address.begin;
  uint64_t maximum_byte_length = virtual_address_span / 2;
  if (maximum_byte_length > SIZE_MAX / 2) {
    maximum_byte_length = SIZE_MAX / 2;
  }
  return maximum_byte_length & ~(uint64_t)(device->page_size - 1);
}

static uint64_t amdf_gpu_kfd_maximum_alignment(
    const amdf_gpu_umd_device_t* device) {
  uint64_t limit = amdf_gpu_kfd_maximum_byte_length(device);
  uint64_t alignment = 1;
  while (alignment <= limit / 2) alignment <<= 1;
  return alignment;
}

static uint32_t amdf_gpu_kfd_address_bit_count(uint64_t maximum_address) {
  uint32_t bit_count = 0;
  do {
    ++bit_count;
    maximum_address >>= 1;
  } while (maximum_address != 0);
  return bit_count;
}

amdf_status_t amdf_gpu_umd_device_query_memory_profile(
    amdf_gpu_umd_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  amdf_memory_profile_t profile = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      .structure_size = out_profile->structure_size,
      .next = out_profile->next,
      .ordinal = memory_profile_ordinal,
      .address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU,
  };
  const uint64_t maximum_byte_length = amdf_gpu_kfd_maximum_byte_length(device);
  const uint64_t maximum_alignment = amdf_gpu_kfd_maximum_alignment(device);
  const amdf_memory_construction_capabilities_t allocation = {
      .maximum_byte_length = maximum_byte_length,
      .byte_length_granularity = 1,
      .minimum_alignment = device->page_size,
      .maximum_alignment = maximum_alignment,
      .native_byte_length_granularity = device->page_size,
  };
  const amdf_memory_construction_capabilities_t registration = {
      .maximum_byte_length = maximum_byte_length,
      .byte_length_granularity = 1,
      .registered_host_pointer_alignment = 1,
      .minimum_alignment = 1,
      .maximum_alignment = maximum_alignment,
      .native_byte_length_granularity = device->page_size,
  };
  const amdf_memory_address_capabilities_t page_address = {
      .address_domain_ordinal = 0,
      .address_bit_count = amdf_gpu_kfd_address_bit_count(
          device->topology.virtual_address.end - 1),
      .minimum_address = device->topology.virtual_address.begin,
      .maximum_address = device->topology.virtual_address.end - 1,
      .minimum_alignment = device->page_size,
  };
  const amdf_host_mapping_capabilities_t host_mapping = {
      .maximum_byte_length = maximum_byte_length,
      .byte_offset_granularity = 1,
      .byte_length_granularity = 1,
      .supported_access =
          AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
  };
  uint32_t ordinal = 0;
  if (memory_profile_ordinal == ordinal++) {
    profile.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    profile.roles = AMDF_MEMORY_PROFILE_ROLE_CREATE |
                    AMDF_MEMORY_PROFILE_ROLE_EXPORT |
                    AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.guaranteed_flags =
        AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_SHAREABLE |
        AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags =
        profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
    profile.supported_device_access = AMDF_MEMORY_ACCESS_READ |
                                      AMDF_MEMORY_ACCESS_WRITE |
                                      AMDF_MEMORY_ACCESS_EXECUTE;
    profile.device_address = page_address;
    profile.allocation = allocation;
    profile.host_mapping = host_mapping;
    profile.external_memory_support_count = 1;
    profile.external_memory_support[0] = (amdf_external_memory_support_t){
        .type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD,
        .flags = AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS,
        .source_offset_alignment = 1,
        .byte_length_alignment = 1,
    };
  } else if ((device->topology.memory_features &
              AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) != 0 &&
             memory_profile_ordinal == ordinal++) {
    profile.memory_class = AMDF_MEMORY_CLASS_LOCAL;
    profile.roles = AMDF_MEMORY_PROFILE_ROLE_CREATE;
    profile.guaranteed_flags =
        AMDF_MEMORY_FLAG_DEVICE_LOCAL | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags =
        profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    if ((device->topology.memory_features &
         AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY) != 0) {
      profile.roles |= AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      profile.supported_flags |= AMDF_MEMORY_FLAG_HOST_VISIBLE;
    }
    profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
    profile.supported_device_access = AMDF_MEMORY_ACCESS_READ |
                                      AMDF_MEMORY_ACCESS_WRITE |
                                      AMDF_MEMORY_ACCESS_EXECUTE;
    profile.device_address = page_address;
    profile.allocation = allocation;
    if ((profile.roles & AMDF_MEMORY_PROFILE_ROLE_HOST_MAP) != 0) {
      profile.host_mapping = host_mapping;
    }
  } else if (device->native_lifetime == AMDF_NATIVE_LIFETIME_PROCESS &&
             memory_profile_ordinal == ordinal) {
    profile.memory_class = AMDF_MEMORY_CLASS_REGISTERED_HOST;
    profile.roles =
        AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.guaranteed_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                               AMDF_MEMORY_FLAG_HOST_COHERENT |
                               AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags =
        profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
    profile.supported_device_access = AMDF_MEMORY_ACCESS_READ |
                                      AMDF_MEMORY_ACCESS_WRITE |
                                      AMDF_MEMORY_ACCESS_EXECUTE;
    profile.device_address = page_address;
    profile.device_address.minimum_alignment = 1;
    profile.registration = registration;
    profile.host_mapping = host_mapping;
  } else {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  *out_profile = profile;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_memory_prepare_import(
    amdf_gpu_umd_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_gpu_umd_memory_t** memory_state,
    amdf_gpu_umd_memory_result_t* out_result) {
  (void)device;
  (void)profile;
  (void)import_info;
  (void)external_memory;
  (void)memory_state;
  (void)out_result;
  return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
}

amdf_status_t amdf_gpu_umd_memory_export(
    amdf_gpu_umd_memory_t* memory, const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value) {
  (void)export_info;
  int descriptor = -1;
  amdf_linux_dma_buf_info_t info;
  amdf_status_t status =
      amdf_gpu_kfd_memory_open_dma_buf(memory, &descriptor, &info);
  if (amdf_status_is_ok(status) &&
      (info.byte_length != memory->byte_length ||
       !amdf_physical_memory_id_is_equal(&info.physical_backing_id,
                                         &memory->physical_backing_id))) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  if (amdf_status_is_ok(status)) {
    *out_value = (amdf_external_memory_t){
        .payload.file_descriptor = descriptor,
        .release = amdf_linux_dma_buf_release,
    };
  } else {
    const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
    if (!amdf_status_is_ok(close_status)) status = close_status;
  }
  return status;
}

amdf_status_t amdf_gpu_umd_memory_describe_site(
    amdf_gpu_umd_memory_t* memory, const amdf_memory_site_query_t* query,
    amdf_memory_site_description_t* out_description) {
  (void)memory;
  const amdf_queue_family_info_t* family = query->queue_family_info;
  const bool qualified_command_format =
      (family->command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_PM4 &&
       family->format_version == AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1) ||
      (family->command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA &&
       family->format_version == AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1);
  if (!qualified_command_format ||
      (family->roles & AMDF_QUEUE_ROLE_CACHE_CONTROL) == 0 ||
      (family->cache_operations &
       (AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
        AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM)) !=
          (AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
           AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM) ||
      (family->cache_transition_kinds & AMDF_CACHE_TRANSITION_KINDS_GLOBAL) ==
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_memory_site_description_t description = {0};
  if ((query->memory_info->device_access & AMDF_MEMORY_ACCESS_READ) != 0) {
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_READ;
  }
  if ((query->memory_info->device_access & AMDF_MEMORY_ACCESS_WRITE) != 0) {
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_WRITE;
  }
  description.release = (amdf_cache_transition_t){
      .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
      .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
      .operation = AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM,
  };
  description.acquire = (amdf_cache_transition_t){
      .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
      .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
      .operation = AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM,
  };
  *out_description = description;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_memory_prepare(
    amdf_gpu_umd_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info,
    amdf_gpu_umd_memory_t** memory_state,
    amdf_gpu_umd_memory_result_t* out_result) {
  amdf_gpu_kfd_memory_plan_t plan = {0};
  amdf_gpu_kfd_memory_plan(device, profile, create_info, &plan);
  amdf_gpu_umd_memory_t* memory = NULL;
  amdf_status_t status =
      amdf_calloc(device->host_allocator, sizeof(*memory),
                  amdf_alignof(amdf_gpu_umd_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  memory->device = device;
  *memory_state = memory;
  memory->byte_length = plan.byte_length;
  const bool registered =
      profile->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST;
  const amdf_gpu_kfd_buffer_create_info_t buffer_create_info = {
      .native_flags = plan.native_flags,
      .byte_length = plan.byte_length,
      .alignment = plan.alignment,
      .host_access = registered
                         ? AMDF_GPU_KFD_BUFFER_HOST_ACCESS_BORROWED
                         : ((plan.flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0
                                ? AMDF_GPU_KFD_BUFFER_HOST_ACCESS_MAPPED
                                : AMDF_GPU_KFD_BUFFER_HOST_ACCESS_NONE),
      .host_pointer = create_info->registered_host_pointer,
      .host_byte_offset = plan.host_byte_offset,
  };
  amdf_gpu_kfd_buffer_result_t buffer_result = {0};
  status = amdf_gpu_kfd_buffer_prepare(device, &buffer_create_info,
                                       &memory->buffer, &buffer_result);
  if (amdf_status_is_ok(status)) {
    memory->host_pointer = buffer_result.host_pointer;
    memory->cacheability = (plan.flags & AMDF_MEMORY_FLAG_HOST_COHERENT) != 0
                               ? AMDF_HOST_CACHEABILITY_WRITE_BACK
                               : AMDF_HOST_CACHEABILITY_WRITE_COMBINED;
  }
  if (amdf_status_is_ok(status) &&
      profile->memory_class == AMDF_MEMORY_CLASS_SYSTEM) {
    amdf_linux_dma_buf_info_t info;
    status = amdf_gpu_kfd_memory_query_dma_buf(memory, &info);
    if (amdf_status_is_ok(status)) {
      memory->physical_backing_id = info.physical_backing_id;
    }
  }
  if (amdf_status_is_ok(status)) {
    // A subpage registration promises only the alignment shared by the caller
    // address and the GPU view, and never exposes unborrowed neighboring bytes.
    const uint64_t alignment =
        registered && plan.host_byte_offset != 0
            ? plan.host_byte_offset & -plan.host_byte_offset
            : plan.alignment;
    const uint64_t length =
        registered ? create_info->byte_length : plan.byte_length;
    const amdf_physical_memory_id_t physical_backing_id =
        profile->memory_class == AMDF_MEMORY_CLASS_SYSTEM
            ? memory->physical_backing_id
            : (amdf_physical_memory_id_t){
                  .words = {registered ? (uintptr_t)memory->host_pointer -
                                             plan.host_byte_offset
                                       : (uintptr_t)memory,
                            plan.byte_length},
              };
    *out_result = (amdf_gpu_umd_memory_result_t){
        .flags = plan.flags,
        .source_byte_offset = registered ? plan.host_byte_offset : 0,
        .byte_length = length,
        .alignment = alignment,
        .native_allocation_byte_length = plan.byte_length,
        .native_allocation_granularity = device->page_size,
        .physical_backing_id = physical_backing_id,
        .device_address = buffer_result.device_address,
    };
  }
  return status;
}

amdf_status_t amdf_gpu_umd_memory_map(
    amdf_gpu_umd_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_map_info_t* map_info,
    amdf_gpu_umd_host_mapping_t** out_mapping,
    amdf_gpu_umd_host_mapping_result_t* out_result) {
  const bool write_back =
      memory->cacheability == AMDF_HOST_CACHEABILITY_WRITE_BACK;
  const uint32_t line_size = write_back ? memory->device->cache_line_size : 0;
  const amdf_cache_transition_t flush = {
      .kind = write_back ? AMDF_CACHE_TRANSITION_KIND_RANGE
                         : AMDF_CACHE_TRANSITION_KIND_GLOBAL,
      .executor = AMDF_CACHE_TRANSITION_EXECUTOR_HOST_DIRECT,
      .host_operation = AMDF_HOST_CACHE_OPERATION_FLUSH,
      .host_instruction = write_back ? AMDF_HOST_CACHE_INSTRUCTION_X86_CLFLUSH
                                     : AMDF_HOST_CACHE_INSTRUCTION_NONE,
      .host_fence_before = write_back ? AMDF_HOST_CACHE_FENCE_X86_MFENCE
                                      : AMDF_HOST_CACHE_FENCE_NONE,
      .host_fence_after = AMDF_HOST_CACHE_FENCE_X86_MFENCE,
      .range_granularity = line_size,
  };
  amdf_cache_transition_t invalidate = flush;
  invalidate.host_operation = AMDF_HOST_CACHE_OPERATION_INVALIDATE;
  *out_result = (amdf_gpu_umd_host_mapping_result_t){
      .flags = profile->host_mapping.supported_access,
      .pointer = (uint8_t*)memory->host_pointer + map_info->byte_offset,
      .byte_length = map_info->byte_length,
      .cacheability = memory->cacheability,
      .cache_line_size = line_size,
      .flush = flush,
      .invalidate = invalidate,
  };
  // The common host-view object owns the borrow; the native mapping persists
  // with memory and needs no separate allocation or per-view native resource.
  *out_mapping = (amdf_gpu_umd_host_mapping_t*)memory;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_host_mapping_cache_control(
    amdf_gpu_umd_host_mapping_t* mapping, amdf_host_cache_operation_t operation,
    uint64_t memory_byte_offset, uint64_t byte_length) {
  const amdf_gpu_umd_memory_t* memory = (amdf_gpu_umd_memory_t*)mapping;
  (void)operation;
  if (memory->cacheability == AMDF_HOST_CACHEABILITY_WRITE_BACK) {
    // GPU snooping does not make this backing coherent with other consumers.
    // Explicit CPU operations cover the requested bytes even on coherent GTT.
    amdf_linux_host_cache_transfer(
        (uint8_t*)memory->host_pointer + memory_byte_offset, byte_length,
        memory->device->cache_line_size);
  } else if (byte_length != 0) {
    // WC VRAM views need store-buffer ordering, not a CPU cache-line flush.
    _mm_mfence();
  }
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_host_mapping_destroy(
    amdf_gpu_umd_host_mapping_t* mapping) {
  (void)mapping;
  return AMDF_STATUS_OK;
}
