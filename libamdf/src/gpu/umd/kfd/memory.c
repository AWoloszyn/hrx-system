// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/memory.h"

#include <emmintrin.h>
#include <linux/kfd_ioctl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "libamdf/src/gpu/umd/kfd/device.h"
#include "libamdf/src/platform/linux/file.h"

// One backing handle and GPU attachment, including incomplete teardown state.
struct amdf_gpu_umd_memory_t {
  // Device borrowed while memory metadata is live.
  amdf_gpu_umd_device_t* device;
  // Native KFD allocation identity, or zero after release.
  uint64_t handle;
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
  plan->native_flags = KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
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
  free(memory);
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

amdf_status_t amdf_gpu_umd_memory_create(
    amdf_gpu_umd_device_t* device, const amdf_memory_create_info_t* create_info,
    amdf_gpu_umd_memory_t** out_memory,
    amdf_gpu_umd_memory_result_t* out_result) {
  *out_memory = NULL;
  amdf_gpu_kfd_memory_plan_t plan = {0};
  amdf_status_t status = amdf_gpu_kfd_memory_plan(device, create_info, &plan);
  if (!amdf_status_is_ok(status)) return status;
  amdf_gpu_umd_memory_t* memory = calloc(1, sizeof(*memory));
  if (memory == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  }
  memory->device = device;
  uint64_t address = 0;
  status = amdf_gpu_kfd_memory_allocate(&plan, create_info, memory, &address);
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
    *out_result = (amdf_gpu_umd_memory_result_t){
        .memory_class = create_info->memory_class,
        .flags = plan.flags,
        .byte_length = length,
        .alignment = alignment,
        .physical_backing_id =
            {
                .words = {registered ? (uintptr_t)memory->host_pointer
                                     : (uintptr_t)memory,
                          length},
            },
        .device_address = address,
    };
    *out_memory = memory;
  } else {
    const amdf_status_t release_status = amdf_gpu_umd_memory_destroy(memory);
    if (!amdf_status_is_ok(release_status)) {
      free(memory);
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
