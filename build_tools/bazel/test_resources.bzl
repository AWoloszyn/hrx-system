# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Physical-resource serialization shared by component test policies."""

# APIs sharing the assigned GPU serialize within one test invocation. Native
# GPU/XDNA interop also coordinates XDNA users through this coarse group.
# Independent invocations still require exclusive device assignment by the job.
GPU_DEVICE_RESOURCE_GROUP = "gpu-device"
