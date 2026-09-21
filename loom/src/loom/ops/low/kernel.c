// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/low/kernel.h"

#include "loom/ops/low/ops.h"

bool loom_low_kernel_def_static_workgroup_size(
    const loom_op_t* op, loom_target_workgroup_size_t* out_size) {
  *out_size = (loom_target_workgroup_size_t){0};
  if (!loom_low_kernel_def_isa(op) ||
      !loom_low_kernel_def_has_workgroup_size_x(op)) {
    return false;
  }
  *out_size = (loom_target_workgroup_size_t){
      .x = (uint32_t)loom_low_kernel_def_workgroup_size_x(op),
      .y = (uint32_t)loom_low_kernel_def_workgroup_size_y(op),
      .z = (uint32_t)loom_low_kernel_def_workgroup_size_z(op),
  };
  return true;
}

bool loom_low_kernel_def_static_workgroup_count(
    const loom_op_t* op, loom_target_dispatch_workgroup_count_t* out_count) {
  *out_count = (loom_target_dispatch_workgroup_count_t){0};
  if (!loom_low_kernel_def_isa(op) ||
      !loom_low_kernel_def_has_workgroup_count_x(op)) {
    return false;
  }
  *out_count = (loom_target_dispatch_workgroup_count_t){
      .x = (uint32_t)loom_low_kernel_def_workgroup_count_x(op),
      .y = (uint32_t)loom_low_kernel_def_workgroup_count_y(op),
      .z = (uint32_t)loom_low_kernel_def_workgroup_count_z(op),
  };
  return true;
}

bool loom_low_kernel_def_static_workgroup_cluster_size(
    const loom_op_t* op, loom_target_workgroup_cluster_size_t* out_size) {
  *out_size = (loom_target_workgroup_cluster_size_t){0};
  if (!loom_low_kernel_def_isa(op) ||
      !loom_low_kernel_def_has_workgroup_cluster_size_x(op)) {
    return false;
  }
  *out_size = (loom_target_workgroup_cluster_size_t){
      .x = (uint32_t)loom_low_kernel_def_workgroup_cluster_size_x(op),
      .y = (uint32_t)loom_low_kernel_def_workgroup_cluster_size_y(op),
      .z = (uint32_t)loom_low_kernel_def_workgroup_cluster_size_z(op),
  };
  return true;
}
