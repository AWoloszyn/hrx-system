// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/memory.h"

#include <assert.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/kfd_ioctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/gpu/umd/kfd/device.h"
#include "libamdf/src/platform/linux/dma_buf.h"
#include "libamdf/src/platform/linux/file.h"

// One backing handle and GPU attachment, including incomplete teardown state.
struct amdf_gpu_umd_memory_t {
  // Device borrowed while memory metadata is live.
  amdf_gpu_umd_device_t* device;
  // Native KFD allocation identity, or zero after release.
  uint64_t handle;
  // Page-covered native backing length in bytes.
  size_t byte_length;
  // Canonical DMA-BUF identity for shareable owned GTT.
  amdf_physical_memory_id_t physical_backing_id;
  // True until the GPU mapping and its unmap synchronization have completed.
  bool mapped;
  // Native unmap progress retained across an interrupted final synchronization.
  uint32_t unmap_success_count;
  // Owned CPU VA interval reserving the GPU VA and any CPU backing mapping.
  struct {
    // First reserved host address, or NULL after release.
    void* base;
    // Complete reserved interval length, including alignment padding.
    size_t byte_length;
  } reservation;
  // Borrowed caller pages or persistent mapping into the owned reservation.
  void* host_pointer;
  // Cache behavior of the host view.
  amdf_host_cacheability_t cacheability;
};

// Native allocation plan produced at the public memory-parameter boundary.
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

static amdf_status_t amdf_gpu_kfd_memory_plan(
    const amdf_gpu_umd_device_t* device,
    const amdf_memory_create_info_t* create_info,
    amdf_gpu_kfd_memory_plan_t* plan) {
  plan->flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  // KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE uses a signed shift into bit 31 in the
  // UAPI header. Construct that bit with an unsigned operand for defined C.
  plan->native_flags = UINT32_C(1) << 31;
  if (create_info->memory_class == AMDF_MEMORY_CLASS_LOCAL) {
    if ((device->topology.memory_features &
         AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) == 0) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }
    plan->flags |= AMDF_MEMORY_FLAG_DEVICE_LOCAL;
    plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_VRAM;
    if ((create_info->required_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0) {
      if ((device->topology.memory_features &
           AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY) == 0) {
        return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
      }
      plan->flags |= AMDF_MEMORY_FLAG_HOST_VISIBLE;
      plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC;
    }
  } else {
    plan->flags |=
        AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_HOST_COHERENT;
    plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
    if (create_info->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST) {
      if (device->mode != AMDF_GPU_DEVICE_MODE_PROCESS) {
        return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
      }
      const uintptr_t pointer = (uintptr_t)create_info->registered_host_pointer;
      if (create_info->minimum_alignment != 0 &&
          (pointer & (create_info->minimum_alignment - 1)) != 0) {
        return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
      }
      if (create_info->byte_length > UINTPTR_MAX - pointer) {
        return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
      }
      plan->host_byte_offset = pointer & (device->page_size - 1);
      plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_USERPTR;
    } else {
      plan->flags |= AMDF_MEMORY_FLAG_SHAREABLE;
      plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_GTT;
    }
  }
  if ((create_info->required_flags & AMDF_MEMORY_FLAG_EXECUTABLE) != 0) {
    plan->flags |= AMDF_MEMORY_FLAG_EXECUTABLE;
    plan->native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
  }
  if ((create_info->required_flags & AMDF_MEMORY_FLAG_QUEUE_STORAGE) != 0) {
    plan->flags |= AMDF_MEMORY_FLAG_QUEUE_STORAGE;
  }
  if ((create_info->required_flags & ~plan->flags) != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const size_t padding = plan->host_byte_offset + device->page_size - 1;
  if (create_info->byte_length > SIZE_MAX - padding ||
      create_info->minimum_alignment > SIZE_MAX) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  plan->byte_length =
      (create_info->byte_length + padding) & ~(device->page_size - 1);
  plan->alignment = create_info->minimum_alignment > device->page_size
                        ? (size_t)create_info->minimum_alignment
                        : device->page_size;
  if (plan->byte_length > SIZE_MAX - (plan->alignment - device->page_size)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_memory_destroy(amdf_gpu_umd_memory_t* memory) {
  if (memory->mapped) {
    struct kfd_ioctl_unmap_memory_from_gpu_args unmap = {
        .handle = memory->handle,
        .device_ids_array_ptr = (uintptr_t)&memory->device->topology.gpu_id,
        .n_devices = 1,
        .n_success = memory->unmap_success_count,
    };
    const int result = ioctl(memory->device->descriptor,
                             AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &unmap);
    memory->unmap_success_count = unmap.n_success;
    if (result != 0) return amdf_linux_error(errno);
    if (unmap.n_success != 1) return amdf_linux_error(EPROTO);
    memory->mapped = false;
  }
  if (memory->handle != 0) {
    struct kfd_ioctl_free_memory_of_gpu_args release = {.handle =
                                                            memory->handle};
    if (ioctl(memory->device->descriptor, AMDKFD_IOC_FREE_MEMORY_OF_GPU,
              &release) != 0) {
      return amdf_linux_error(errno);
    }
    memory->handle = 0;
  }
  if (memory->reservation.base != NULL) {
    if (munmap(memory->reservation.base, memory->reservation.byte_length) !=
        0) {
      return amdf_linux_error(errno);
    }
    memory->reservation.base = NULL;
  }
  amdf_free(memory->device->host_allocator, memory);
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_gpu_kfd_memory_allocate(
    const amdf_gpu_kfd_memory_plan_t* plan,
    const amdf_memory_create_info_t* create_info, amdf_gpu_umd_memory_t* memory,
    uint64_t* out_device_address) {
  memory->reservation.byte_length =
      plan->byte_length + plan->alignment - memory->device->page_size;
  void* reservation = mmap(NULL, memory->reservation.byte_length, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED) return amdf_linux_error(errno);
  memory->reservation.base = reservation;
  const uintptr_t address =
      ((uintptr_t)reservation + plan->alignment - 1) & ~(plan->alignment - 1);
  const amdf_gpu_kfd_topology_t* topology = &memory->device->topology;
  if (address < topology->virtual_address.begin ||
      address >= topology->virtual_address.end ||
      plan->byte_length > topology->virtual_address.end - address) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }

  struct kfd_ioctl_alloc_memory_of_gpu_args allocate = {
      .va_addr = address,
      .size = plan->byte_length,
      .gpu_id = topology->gpu_id,
      .flags = plan->native_flags,
  };
  if (create_info->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST) {
    memory->host_pointer = create_info->registered_host_pointer;
    allocate.mmap_offset =
        (uintptr_t)memory->host_pointer - plan->host_byte_offset;
  }
  if (ioctl(memory->device->descriptor, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU,
            &allocate) != 0) {
    return amdf_linux_error(errno);
  }
  memory->handle = allocate.handle;
  if (memory->handle == 0) return amdf_linux_error(EPROTO);

  if (create_info->memory_class != AMDF_MEMORY_CLASS_REGISTERED_HOST &&
      (plan->flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0) {
    // MAP_FIXED only replaces pages in this object's own PROT_NONE reservation.
    void* mapping =
        mmap((void*)address, plan->byte_length, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, memory->device->render_descriptor,
             allocate.mmap_offset);
    if (mapping == MAP_FAILED) return amdf_linux_error(errno);
    memory->host_pointer = mapping;
  }
  memory->cacheability = (plan->flags & AMDF_MEMORY_FLAG_HOST_COHERENT) != 0
                             ? AMDF_HOST_CACHEABILITY_COHERENT
                             : AMDF_HOST_CACHEABILITY_WRITE_COMBINED;
  struct kfd_ioctl_map_memory_to_gpu_args map = {
      .handle = memory->handle,
      .device_ids_array_ptr = (uintptr_t)&topology->gpu_id,
      .n_devices = 1,
  };
  const int result =
      ioctl(memory->device->descriptor, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &map);
  // Mapping can succeed before the final residency/page-table wait fails.
  memory->mapped = map.n_success != 0;
  if (result != 0) return amdf_linux_error(errno);
  if (map.n_success != 1) return amdf_linux_error(EPROTO);
  *out_device_address = address + plan->host_byte_offset;
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_gpu_kfd_memory_open_dma_buf(
    amdf_gpu_umd_memory_t* memory, int* out_descriptor,
    amdf_linux_dma_buf_info_t* out_info) {
  struct kfd_ioctl_export_dmabuf_args export_args = {
      .handle = memory->handle,
      .flags = O_CLOEXEC,
  };
  if (ioctl(memory->device->descriptor, AMDKFD_IOC_EXPORT_DMABUF,
            &export_args) != 0) {
    return amdf_linux_error(errno);
  }
  assert(export_args.dmabuf_fd <= INT_MAX &&
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

static uint32_t amdf_gpu_kfd_memory_profile_ordinal(
    const amdf_gpu_umd_device_t* device, amdf_memory_class_t memory_class) {
  if (memory_class == AMDF_MEMORY_CLASS_SYSTEM) return 0;
  const bool local_memory_supported =
      (device->topology.memory_features &
       AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) != 0;
  if (memory_class == AMDF_MEMORY_CLASS_LOCAL) {
    assert(local_memory_supported &&
           "created local memory must have an advertised profile");
    return 1;
  }
  assert(memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST &&
         device->mode == AMDF_GPU_DEVICE_MODE_PROCESS &&
         "registered memory must have an advertised profile");
  return local_memory_supported ? 2 : 1;
}

amdf_status_t amdf_gpu_umd_device_query_memory_profile(
    amdf_gpu_umd_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  amdf_memory_profile_t profile = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      .structure_size = out_profile->structure_size,
      .next = out_profile->next,
      .ordinal = memory_profile_ordinal,
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
    profile.supported_flags = profile.guaranteed_flags |
                              AMDF_MEMORY_FLAG_EXECUTABLE |
                              AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    profile.minimum_alignment = device->page_size;
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
    profile.supported_flags = profile.guaranteed_flags |
                              AMDF_MEMORY_FLAG_EXECUTABLE |
                              AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    if ((device->topology.memory_features &
         AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY) != 0) {
      profile.roles |= AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      profile.supported_flags |= AMDF_MEMORY_FLAG_HOST_VISIBLE;
    }
    profile.minimum_alignment = device->page_size;
  } else if (device->mode == AMDF_GPU_DEVICE_MODE_PROCESS &&
             memory_profile_ordinal == ordinal) {
    profile.memory_class = AMDF_MEMORY_CLASS_REGISTERED_HOST;
    profile.roles =
        AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.guaranteed_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                               AMDF_MEMORY_FLAG_HOST_COHERENT |
                               AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags = profile.guaranteed_flags |
                              AMDF_MEMORY_FLAG_EXECUTABLE |
                              AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    profile.minimum_alignment = 1;
  } else {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  *out_profile = profile;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_memory_import(
    amdf_gpu_umd_device_t* device, const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_gpu_umd_memory_t** out_memory,
    amdf_gpu_umd_memory_result_t* out_result) {
  (void)device;
  (void)import_info;
  (void)external_memory;
  (void)out_memory;
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

amdf_status_t amdf_gpu_umd_memory_query_pair_info(
    amdf_gpu_umd_memory_t* memory, const amdf_memory_pair_query_t* query,
    amdf_memory_pair_info_t* out_info) {
  (void)memory;
  (void)query;
  (void)out_info;
  return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
}

amdf_status_t amdf_gpu_umd_memory_create(
    amdf_gpu_umd_device_t* device, const amdf_memory_create_info_t* create_info,
    amdf_gpu_umd_memory_t** out_memory,
    amdf_gpu_umd_memory_result_t* out_result) {
  amdf_gpu_kfd_memory_plan_t plan = {0};
  amdf_status_t status = amdf_gpu_kfd_memory_plan(device, create_info, &plan);
  if (!amdf_status_is_ok(status)) return status;
  amdf_gpu_umd_memory_t* memory = NULL;
  status = amdf_calloc(device->host_allocator, sizeof(*memory),
                       _Alignof(amdf_gpu_umd_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  memory->device = device;
  memory->byte_length = plan.byte_length;
  uint64_t address = 0;
  status = amdf_gpu_kfd_memory_allocate(&plan, create_info, memory, &address);
  if (amdf_status_is_ok(status) &&
      create_info->memory_class == AMDF_MEMORY_CLASS_SYSTEM) {
    amdf_linux_dma_buf_info_t info;
    status = amdf_gpu_kfd_memory_query_dma_buf(memory, &info);
    if (amdf_status_is_ok(status)) {
      memory->physical_backing_id = info.physical_backing_id;
    }
  }
  if (amdf_status_is_ok(status)) {
    const bool registered =
        create_info->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST;
    // A subpage registration promises only the alignment shared by the caller
    // address and the GPU view, and never exposes unborrowed neighboring bytes.
    const uint64_t alignment =
        registered && plan.host_byte_offset != 0
            ? plan.host_byte_offset & -plan.host_byte_offset
            : plan.alignment;
    const uint64_t length =
        registered ? create_info->byte_length : plan.byte_length;
    const amdf_physical_memory_id_t physical_backing_id =
        create_info->memory_class == AMDF_MEMORY_CLASS_SYSTEM
            ? memory->physical_backing_id
            : (amdf_physical_memory_id_t){
                  .words = {registered ? (uintptr_t)memory->host_pointer
                                       : (uintptr_t)memory,
                            length},
              };
    *out_result = (amdf_gpu_umd_memory_result_t){
        .memory_profile_ordinal = amdf_gpu_kfd_memory_profile_ordinal(
            device, create_info->memory_class),
        .memory_class = create_info->memory_class,
        .flags = plan.flags,
        .source_byte_offset = 0,
        .byte_length = length,
        .alignment = alignment,
        .physical_backing_id = physical_backing_id,
        .device_address = address,
    };
    *out_memory = memory;
  } else {
    const amdf_status_t release_status = amdf_gpu_umd_memory_destroy(memory);
    if (!amdf_status_is_ok(release_status)) {
      amdf_free(device->host_allocator, memory);
      status = release_status;
    }
  }
  return status;
}

amdf_status_t amdf_gpu_umd_memory_map(
    amdf_gpu_umd_memory_t* memory, const amdf_memory_map_info_t* map_info,
    amdf_gpu_umd_host_mapping_t** out_mapping,
    amdf_gpu_umd_host_mapping_result_t* out_result) {
  *out_result = (amdf_gpu_umd_host_mapping_result_t){
      .flags = map_info->flags,
      .pointer = (uint8_t*)memory->host_pointer + map_info->byte_offset,
      .byte_length = map_info->byte_length,
      .cacheability = memory->cacheability,
      .cache_line_size = memory->device->cache_line_size,
  };
  // The common host-view object owns the borrow; the native mapping persists
  // with memory and needs no separate allocation or per-view native resource.
  *out_mapping = (amdf_gpu_umd_host_mapping_t*)memory;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_host_mapping_cache_control(
    amdf_gpu_umd_host_mapping_t* mapping, amdf_host_cache_operation_t operation,
    uint64_t byte_offset, uint64_t byte_length) {
  (void)mapping;
  (void)operation;
  (void)byte_offset;
  // x86 coherent system pages need no host cache-line operation. WC VRAM
  // mappings still require store-buffer ordering at a host ownership boundary.
  if (byte_length != 0) _mm_mfence();
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_umd_host_mapping_destroy(
    amdf_gpu_umd_host_mapping_t* mapping) {
  (void)mapping;
  return AMDF_STATUS_OK;
}
