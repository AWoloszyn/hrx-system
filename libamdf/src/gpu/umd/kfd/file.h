// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_FILE_H_
#define AMDF_SRC_GPU_UMD_KFD_FILE_H_

#include <fcntl.h>
#include <linux/kfd_ioctl.h>
#include <sys/ioctl.h>

#include "libamdf/src/platform/linux/file.h"

// Opens one owned KFD file and queries its native ABI. On failure the caller
// still owns any successfully opened descriptor and releases it explicitly.
static inline amdf_status_t amdf_gpu_kfd_file_open(
    int* descriptor, struct kfd_ioctl_get_version_args* version) {
  *descriptor = open("/dev/kfd", O_RDWR | O_CLOEXEC);
  if (*descriptor < 0) return amdf_linux_error(errno);
  if (ioctl(*descriptor, AMDKFD_IOC_GET_VERSION, version) != 0) {
    return amdf_linux_error(errno);
  }
  return AMDF_STATUS_OK;
}

#endif  // AMDF_SRC_GPU_UMD_KFD_FILE_H_
