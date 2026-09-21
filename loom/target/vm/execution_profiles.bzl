# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Reference function execution through the IREE VM provider."""

load("//loom/build_tools/bazel:defs.bzl", "loom_execution_profile")
load("//loom/requirements:defs.bzl", "TARGET_ARCH_VM")

VM_REFERENCE_PROFILE = loom_execution_profile(
    name = "vm_reference",
    build_requirements = [TARGET_ARCH_VM],
    executor = "reference",
    target_class = "cpu",
    target_family = "vm",
)
