# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Public execution profiles owned by the Loom AMDGPU target provider."""

load("//loom/build_tools/bazel:defs.bzl", "loom_execution_profile")
load(
    "//loom/requirements:defs.bzl",
    "EMIT_AMDGPU",
    "EXECUTE_IREE_HAL",
    "TARGET_ARCH_AMDGPU",
)
load(
    "//runtime/requirements:defs.bzl",
    "AMDGPU_RESOURCE",
    "HAL_AMDGPU",
)

def amdgpu_execution_profile(name, runner_args = [], sanitizer_suppressions = None, tags = []):
    """Defines AMDGPU execution with shared device and resource requirements.

    Callers own instrumentation, diagnostic reporting and suppression policy.
    Workload configuration and case selection remain on the test declaration.
    """
    return loom_execution_profile(
        name = name,
        build_requirements = [
            TARGET_ARCH_AMDGPU,
            EMIT_AMDGPU,
            EXECUTE_IREE_HAL,
            HAL_AMDGPU,
        ],
        executor = "hardware",
        resource_group = "loom-amdgpu-tests",
        run_requirements = [AMDGPU_RESOURCE],
        runner_args = ["--device=amdgpu"] + runner_args,
        sanitizer_suppressions = sanitizer_suppressions,
        tags = tags,
        target_class = "gpu",
        target_family = "amdgpu",
    )

AMDGPU_HARDWARE_PROFILE = amdgpu_execution_profile(
    name = "amdgpu_hardware",
)

AMDGPU_ACCESS_PROFILE = amdgpu_execution_profile(
    name = "amdgpu_access",
    runner_args = ["--sanitizer=access"],
    tags = ["notsan"],
)

AMDGPU_ASAN_PROFILE = amdgpu_execution_profile(
    name = "amdgpu_asan",
    runner_args = [
        "--sanitizer=asan",
        "--sanitizer-reporting=report-only",
        "--amdgpu_asan=true",
        "--amdgpu_asan_report_policy=report-only",
    ],
    tags = ["notsan"],
)

AMDGPU_TSAN_PROFILE = amdgpu_execution_profile(
    name = "amdgpu_tsan",
    runner_args = [
        "--sanitizer=tsan",
        "--sanitizer-reporting=report-only",
        "--amdgpu_tsan=true",
        "--amdgpu_tsan_report_policy=report-only",
    ],
)
