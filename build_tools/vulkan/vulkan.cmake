# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

option(IREE_ENABLE_VULKAN "Build Vulkan API clients independently of HAL drivers" OFF)

set(IREE_VULKAN_AVAILABLE OFF)
if(IREE_ENABLE_VULKAN OR IREE_HAL_DRIVER_VULKAN)
  set(IREE_VULKAN_AVAILABLE ON)
endif()
