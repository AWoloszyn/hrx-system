// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/kfd/topology.h"

#include <dirent.h>
#include <drm/amdgpu_drm.h>
#include <fcntl.h>
#include <linux/kfd_sysfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "libamdf/src/platform/linux/endpoint.h"
#include "libamdf/src/platform/linux/file.h"

static amdf_status_t amdf_gpu_kfd_read_attribute(int directory,
                                                 const char* name, char* text,
                                                 size_t capacity) {
  int descriptor = openat(directory, name, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return amdf_linux_error(errno);
  ssize_t length;
  do {
    length = read(descriptor, text, capacity - 1);
  } while (length < 0 && errno == EINTR);
  amdf_status_t status = length < 0 ? amdf_linux_error(errno) : AMDF_STATUS_OK;
  if (length >= 0) {
    text[length] = 0;
    if ((size_t)length == capacity - 1) status = amdf_linux_error(EOVERFLOW);
  }
  const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
  return amdf_status_is_ok(close_status) ? status : close_status;
}

static amdf_status_t amdf_gpu_kfd_read_number(int directory, const char* name,
                                              uint32_t* out_value) {
  char text[32];
  const amdf_status_t status =
      amdf_gpu_kfd_read_attribute(directory, name, text, sizeof(text));
  if (!amdf_status_is_ok(status)) return status;
  char* end = NULL;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno || value > UINT32_MAX || end == text ||
      (*end != 0 && (*end != '\n' || end[1] != 0))) {
    return amdf_linux_error(EPROTO);
  }
  *out_value = (uint32_t)value;
  return AMDF_STATUS_OK;
}

// KFD topology properties are decimal name/value records. Baseline endpoint
// fields are mandatory, including fields for which zero is meaningful. The
// SDMA group is optional on older kernels but must be complete when present.
static amdf_status_t amdf_gpu_kfd_read_node(
    int directory, const amdf_platform_endpoint_t* endpoint,
    amdf_gpu_kfd_topology_t* topology, bool* out_matches) {
  enum {
    RENDER_MINOR,
    VENDOR_ID,
    DEVICE_ID,
    GFX_TARGET,
    CAPABILITY,
    SIMD_COUNT,
    SIMD_PER_CU,
    WAVES_PER_SIMD,
    SCRATCH_SLOTS,
    WAVE_SIZE,
    LDS_KIB,
    ARRAY_COUNT,
    ARRAYS_PER_ENGINE,
    XCC_COUNT,
    COMPUTE_QUEUE_COUNT,
    CONTEXT_SAVE_RESTORE_SIZE,
    CONTROL_STACK_SIZE,
    REQUIRED_PROPERTY_COUNT,
    SDMA_ENGINE_COUNT = REQUIRED_PROPERTY_COUNT,
    SDMA_XGMI_ENGINE_COUNT,
    SDMA_QUEUE_COUNT_PER_ENGINE,
    PROPERTY_COUNT,
  };
  const char* names[PROPERTY_COUNT] = {
      "drm_render_minor",
      "vendor_id",
      "device_id",
      "gfx_target_version",
      "capability",
      "simd_count",
      "simd_per_cu",
      "max_waves_per_simd",
      "max_slots_scratch_cu",
      "wave_front_size",
      "lds_size_in_kb",
      "array_count",
      "simd_arrays_per_engine",
      "num_xcc",
      "num_cp_queues",
      "cwsr_size",
      "ctl_stack_size",
      "num_sdma_engines",
      "num_sdma_xgmi_engines",
      "num_sdma_queues_per_engine",
  };
  uint32_t values[PROPERTY_COUNT] = {0};
  uint32_t present = 0;
  char text[8192];
  amdf_status_t status =
      amdf_gpu_kfd_read_attribute(directory, "properties", text, sizeof(text));
  if (!amdf_status_is_ok(status)) return status;
  char* cursor = text;
  while (*cursor != 0) {
    char* separator = strchr(cursor, ' ');
    if (separator == NULL) return amdf_linux_error(EPROTO);
    *separator = 0;
    char* end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(separator + 1, &end, 10);
    if (errno || end == separator + 1 || *end != '\n') {
      return amdf_linux_error(EPROTO);
    }
    for (uint32_t i = 0; i < PROPERTY_COUNT; ++i) {
      if (strcmp(cursor, names[i]) != 0) continue;
      if (value > UINT32_MAX || (present & (1u << i)) != 0) {
        return amdf_linux_error(EPROTO);
      }
      values[i] = (uint32_t)value;
      present |= 1u << i;
      break;
    }
    cursor = end + 1;
  }
  if ((present & (1u << RENDER_MINOR)) == 0) {
    return amdf_linux_error(EPROTO);
  }
  if (values[RENDER_MINOR] != (uint32_t)endpoint->info.id.words[0]) {
    return AMDF_STATUS_OK;
  }
  const uint32_t required_properties = (1u << REQUIRED_PROPERTY_COUNT) - 1;
  const uint32_t sdma_properties =
      ((1u << PROPERTY_COUNT) - 1) & ~required_properties;
  if ((present & required_properties) != required_properties ||
      ((present & sdma_properties) != 0 &&
       (present & sdma_properties) != sdma_properties)) {
    return amdf_linux_error(EPROTO);
  }
  if (values[VENDOR_ID] != endpoint->info.pci.vendor_id ||
      values[DEVICE_ID] != endpoint->info.pci.device_id) {
    return amdf_linux_error(ENODEV);
  }
  const uint64_t arrays_per_xcc =
      (uint64_t)values[ARRAYS_PER_ENGINE] * values[XCC_COUNT];
  const uint64_t maximum_waves =
      (uint64_t)values[WAVES_PER_SIMD] * values[SIMD_PER_CU];
  if (values[SIMD_PER_CU] == 0 ||
      values[SIMD_COUNT] % values[SIMD_PER_CU] != 0 || arrays_per_xcc == 0 ||
      values[ARRAY_COUNT] % arrays_per_xcc != 0 || maximum_waves > UINT32_MAX) {
    return amdf_linux_error(EPROTO);
  }
  topology->properties = (amdf_gpu_endpoint_properties_t){
      .gfx_ip =
          {
              .major = values[GFX_TARGET] / 10000,
              .minor = (values[GFX_TARGET] / 100) % 100,
              .stepping = values[GFX_TARGET] % 100,
          },
      .asic_revision = (values[CAPABILITY] & HSA_CAP_ASIC_REVISION_MASK) >>
                       HSA_CAP_ASIC_REVISION_SHIFT,
      .compute =
          {
              .wavefront_size = values[WAVE_SIZE],
              .compute_unit_count = values[SIMD_COUNT] / values[SIMD_PER_CU],
              .maximum_wave_count_per_compute_unit = (uint32_t)maximum_waves,
              .maximum_scratch_wave_count_per_compute_unit =
                  values[SCRATCH_SLOTS],
              .local_data_share_byte_length = (uint64_t)values[LDS_KIB] * 1024,
          },
      .topology =
          {
              .xcc_count = values[XCC_COUNT],
              .shader_engine_count_per_xcc =
                  (uint32_t)(values[ARRAY_COUNT] / arrays_per_xcc),
          },
  };
  topology->compute_queue_count = values[COMPUTE_QUEUE_COUNT];
  topology->sdma.engine_count = values[SDMA_ENGINE_COUNT];
  topology->sdma.xgmi_engine_count = values[SDMA_XGMI_ENGINE_COUNT];
  topology->sdma.queue_count_per_engine = values[SDMA_QUEUE_COUNT_PER_ENGINE];
  topology->context_save_restore_byte_length =
      values[CONTEXT_SAVE_RESTORE_SIZE];
  topology->control_stack_byte_length = values[CONTROL_STACK_SIZE];
  *out_matches = true;
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_gpu_kfd_query_sdma(
    const amdf_platform_endpoint_t* endpoint,
    amdf_gpu_kfd_topology_t* topology) {
  if (topology->sdma.engine_count == 0 &&
      topology->sdma.xgmi_engine_count == 0) {
    return AMDF_STATUS_OK;
  }
  struct drm_amdgpu_info_hw_ip sdma = {0};
  struct drm_amdgpu_info query = {
      .return_pointer = (uintptr_t)&sdma,
      .return_size = sizeof(sdma),
      .query = AMDGPU_INFO_HW_IP_INFO,
      .query_hw_ip =
          {
              .type = AMDGPU_HW_IP_DMA,
              .ip_instance = 0,
          },
  };
  if (ioctl(endpoint->descriptor, DRM_IOCTL_AMDGPU_INFO, &query) != 0) {
    // Older kernels may expose KFD SDMA counts without the exact DRM discovery
    // version. Other GPU services remain usable, but no packet ABI is selected.
    return errno == EINVAL || errno == ENODEV ? AMDF_STATUS_OK
                                              : amdf_linux_error(errno);
  }
  if ((sdma.ip_discovery_version & UINT32_C(0xff000000)) != 0) {
    return amdf_linux_error(EPROTO);
  }
  if (sdma.ip_discovery_version != 0) {
    topology->sdma.ip.major = (sdma.ip_discovery_version >> 16) & 0xff;
    topology->sdma.ip.minor = (sdma.ip_discovery_version >> 8) & 0xff;
    topology->sdma.ip.revision = sdma.ip_discovery_version & 0xff;
    topology->sdma.ip.exact = true;
  }
  return AMDF_STATUS_OK;
}

static amdf_status_t amdf_gpu_kfd_query_memory(
    const amdf_platform_endpoint_t* endpoint,
    amdf_gpu_kfd_topology_t* topology) {
  struct drm_amdgpu_info_device device = {0};
  struct drm_amdgpu_info query = {
      .return_pointer = (uintptr_t)&device,
      .return_size = sizeof(device),
      .query = AMDGPU_INFO_DEV_INFO,
  };
  if (ioctl(endpoint->descriptor, DRM_IOCTL_AMDGPU_INFO, &query) != 0) {
    return amdf_linux_error(errno);
  }
  if (device.device_id != endpoint->info.pci.device_id ||
      device.virtual_address_offset >= device.virtual_address_max ||
      device.virtual_address_alignment == 0 ||
      (device.virtual_address_alignment &
       (device.virtual_address_alignment - 1)) != 0) {
    return amdf_linux_error(EPROTO);
  }
  topology->virtual_address.begin = device.virtual_address_offset;
  topology->virtual_address.end = device.virtual_address_max;
  topology->virtual_address.alignment = device.virtual_address_alignment;
  struct drm_amdgpu_memory_info memory = {0};
  query.return_pointer = (uintptr_t)&memory;
  query.return_size = sizeof(memory);
  query.query = AMDGPU_INFO_MEMORY;
  if (ioctl(endpoint->descriptor, DRM_IOCTL_AMDGPU_INFO, &query) != 0) {
    return amdf_linux_error(errno);
  }
  // APU VRAM requests may be redirected to GTT by KFD. System memory remains
  // available there without promising a physical placement the kernel changes.
  if ((device.ids_flags & AMDGPU_IDS_FLAGS_FUSION) == 0 &&
      memory.vram.total_heap_size != 0) {
    topology->memory_features |= AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY;
    if (memory.cpu_accessible_vram.total_heap_size >=
        memory.vram.total_heap_size) {
      topology->memory_features |=
          AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;
    }
  }
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_kfd_topology_query(
    const amdf_platform_endpoint_t* endpoint,
    amdf_gpu_kfd_topology_t* out_topology) {
  int topology_directory =
      openat(endpoint->instance->sysfs_descriptor, "class/kfd/kfd/topology",
             O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (topology_directory < 0) {
    return errno == ENOENT ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
                           : amdf_linux_error(errno);
  }
  uint32_t generation = 0;
  amdf_status_t status = amdf_gpu_kfd_read_number(topology_directory,
                                                  "generation_id", &generation);
  int nodes = -1;
  if (amdf_status_is_ok(status)) {
    nodes =
        openat(topology_directory, "nodes", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (nodes < 0) status = amdf_linux_error(errno);
  }
  DIR* directory = NULL;
  if (amdf_status_is_ok(status)) {
    directory = fdopendir(nodes);
    if (directory == NULL)
      status = amdf_linux_error(errno);
    else
      nodes = -1;
  }
  amdf_gpu_kfd_topology_t topology = {0};
  bool found = false;
  while (amdf_status_is_ok(status) && !found) {
    errno = 0;
    const struct dirent* entry = readdir(directory);
    if (entry == NULL) {
      if (errno) status = amdf_linux_error(errno);
      break;
    }
    if (entry->d_name[0] == 0 ||
        strspn(entry->d_name, "0123456789") != strlen(entry->d_name))
      continue;
    int node = openat(dirfd(directory), entry->d_name,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (node < 0) {
      status = amdf_linux_error(errno);
      break;
    }
    uint32_t gpu_id = 0;
    status = amdf_gpu_kfd_read_number(node, "gpu_id", &gpu_id);
    if (amdf_status_is_ok(status) && gpu_id != 0) {
      status = amdf_gpu_kfd_read_node(node, endpoint, &topology, &found);
      if (found) topology.gpu_id = gpu_id;
    }
    const amdf_status_t close_status = amdf_linux_file_close(&node);
    if (!amdf_status_is_ok(close_status)) status = close_status;
  }
  uint32_t final_generation = 0;
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_kfd_read_number(topology_directory, "generation_id",
                                      &final_generation);
    if (amdf_status_is_ok(status) && final_generation != generation) {
      status = amdf_linux_error(EAGAIN);
    }
  }
  if (directory != NULL && closedir(directory) != 0)
    status = amdf_linux_error(errno);
  const amdf_status_t nodes_status = amdf_linux_file_close(&nodes);
  if (!amdf_status_is_ok(nodes_status)) status = nodes_status;
  const amdf_status_t close_status = amdf_linux_file_close(&topology_directory);
  if (!amdf_status_is_ok(close_status)) status = close_status;
  if (amdf_status_is_ok(status) && !found) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  if (amdf_status_is_ok(status))
    status = amdf_gpu_kfd_query_memory(endpoint, &topology);
  if (amdf_status_is_ok(status))
    status = amdf_gpu_kfd_query_sdma(endpoint, &topology);
  if (amdf_status_is_ok(status)) *out_topology = topology;
  return status;
}
