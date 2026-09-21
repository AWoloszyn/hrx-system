# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

option(IREE_ENABLE_D3D12 "Build D3D12 API clients for Windows targets" ON)

set(IREE_D3D12_AVAILABLE OFF)
if(IREE_ENABLE_D3D12 AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(IREE_D3D12_AVAILABLE ON)
endif()
