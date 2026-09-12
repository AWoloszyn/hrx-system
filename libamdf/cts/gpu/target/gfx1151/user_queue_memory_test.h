// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_TARGET_GFX1151_USER_QUEUE_MEMORY_TEST_H_
#define AMDF_CTS_GPU_TARGET_GFX1151_USER_QUEUE_MEMORY_TEST_H_

#include "amdf/amdf.h"
#include "amdf/gpu.h"

// Checks one PM4 or SDMA copy and native retirement on a borrowed gfx1151
// device. Reports failures through GoogleTest. Returns whether all queue and
// memory children were released; otherwise the caller must retain the device.
bool RunGfx1151UserQueueMemoryCopies(const amdf_api_t* api,
                                     const amdf_gpu_api_t* gpu_api,
                                     amdf_endpoint_t* endpoint,
                                     amdf_device_t* device,
                                     amdf_queue_command_type_t command_type);

#endif  // AMDF_CTS_GPU_TARGET_GFX1151_USER_QUEUE_MEMORY_TEST_H_
