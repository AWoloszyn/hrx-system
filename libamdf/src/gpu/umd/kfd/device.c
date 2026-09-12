// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/kfd/device.h"

#include "libamdf/src/allocator.h"
#include "libamdf/src/gpu/umd/kfd/file.h"
#include "libamdf/src/gpu/umd/kfd/reset_monitor.h"
#include "libamdf/src/gpu/umd/kfd/target/user_queue.h"
#include "libamdf/src/gpu/umd/kfd/user_queue_native.h"
#include "libamdf/src/platform/linux/endpoint.h"
#include "libamdf/src/platform/linux/host_cache.h"

amdf_status_t amdf_gpu_umd_device_destroy(amdf_gpu_umd_device_t* device) {
  amdf_status_t status =
      amdf_gpu_kfd_vm_bootstrap_release(&device->vm_bootstrap);
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_kfd_reset_monitor_deinitialize(&device->reset_monitor);
  }
  if (amdf_status_is_ok(status)) {
    status = amdf_linux_file_close(&device->descriptor);
  }
  if (amdf_status_is_ok(status)) {
    status = amdf_linux_file_close(&device->render_descriptor);
  }
  if (amdf_status_is_ok(status)) {
    const amdf_allocator_t host_allocator = device->host_allocator;
    amdf_free(host_allocator, device);
  }
  return status;
}

amdf_status_t amdf_gpu_umd_device_create(
    amdf_platform_endpoint_t* endpoint, amdf_allocator_t host_allocator,
    amdf_native_lifetime_t native_lifetime, amdf_gpu_umd_device_t** out_device,
    amdf_gpu_umd_device_result_t* out_result) {
  amdf_gpu_umd_device_t* device = NULL;
  amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*device),
                  amdf_alignof(amdf_gpu_umd_device_t), (void**)&device);
  if (!amdf_status_is_ok(status)) return status;
  device->host_allocator = host_allocator;
  device->descriptor = -1;
  device->render_descriptor = -1;
  device->native_lifetime = native_lifetime;
  device->user_queue_native_api = amdf_gpu_kfd_user_queue_default_native_api();

  status = amdf_gpu_kfd_topology_query(endpoint, &device->topology);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (amdf_status_is_ok(status)) {
    if (page_size <= 0 || (page_size & (page_size - 1)) != 0 ||
        (uint64_t)page_size != device->topology.virtual_address.alignment) {
      status = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    } else {
      device->page_size = (size_t)page_size;
      status = amdf_linux_host_cache_query_line_size(&device->cache_line_size);
    }
  }
  struct kfd_ioctl_get_version_args version = {0};
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_kfd_file_open(&device->descriptor, &version);
  }
  if (amdf_status_is_ok(status) &&
      (version.major_version != 1 || version.minor_version < 18 ||
       (native_lifetime == AMDF_NATIVE_LIFETIME_INSTANCE &&
        version.minor_version < 19))) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  if (amdf_status_is_ok(status) &&
      native_lifetime == AMDF_NATIVE_LIFETIME_INSTANCE) {
    if (ioctl(device->descriptor, AMDKFD_IOC_CREATE_PROCESS, NULL) != 0) {
      status = amdf_linux_error(errno);
    }
  }
  if (amdf_status_is_ok(status)) {
    status =
        amdf_linux_endpoint_open_file(endpoint, &device->render_descriptor);
  }
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_kfd_vm_acquire(
        device->descriptor, device->render_descriptor, &device->topology,
        device->page_size, amdf_gpu_kfd_vm_default_native_api(),
        &device->vm_bootstrap);
  }
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_kfd_vm_bootstrap_release(&device->vm_bootstrap);
  }
  if (amdf_status_is_ok(status)) {
    amdf_gpu_kfd_user_queue_plans_t queue_plans;
    amdf_gpu_kfd_target_user_queue_plans_initialize(
        &device->topology, device->page_size, device->cache_line_size,
        &queue_plans);
    if (queue_plans.count != 0) {
      status = amdf_gpu_kfd_reset_monitor_initialize(
          device->render_descriptor,
          amdf_gpu_kfd_reset_monitor_default_native_api(),
          &device->reset_monitor);
    }
  }
  if (amdf_status_is_ok(status)) {
    *out_result = (amdf_gpu_umd_device_result_t){
        .id = {.words = {endpoint->info.id.words[0], (uintptr_t)device}},
        .reset_epoch = 1,
    };
    *out_device = device;
  } else {
    const amdf_status_t release_status = amdf_gpu_umd_device_destroy(device);
    if (!amdf_status_is_ok(release_status)) {
      // Native cleanup retains any unreleased backing. Only unpublished host
      // metadata is abandoned here; the endpoint owns no retry obligation.
      amdf_free(host_allocator, device);
      status = release_status;
    }
  }
  return status;
}
