// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/platform/linux/instance.h"

#include <fcntl.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/platform/linux/file.h"

amdf_status_t amdf_platform_instance_create(
    amdf_allocator_t host_allocator, amdf_platform_instance_t** out_instance) {
  amdf_platform_instance_t* instance = NULL;
  amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*instance),
                  amdf_alignof(amdf_platform_instance_t), (void**)&instance);
  if (!amdf_status_is_ok(status)) return status;
  instance->host_allocator = host_allocator;
  instance->sysfs_descriptor = open("/sys", O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (instance->sysfs_descriptor < 0) {
    status = amdf_linux_error(errno);
    amdf_free(host_allocator, instance);
    return status;
  }
  *out_instance = instance;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_platform_instance_destroy(
    amdf_platform_instance_t* instance) {
  const amdf_status_t status =
      amdf_linux_file_close(&instance->sysfs_descriptor);
  if (amdf_status_is_ok(status)) {
    const amdf_allocator_t host_allocator = instance->host_allocator;
    amdf_free(host_allocator, instance);
  }
  return status;
}
