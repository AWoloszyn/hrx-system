// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef LIBHRX_SRC_LIBHRX_DEVICE_SELECTION_H_
#define LIBHRX_SRC_LIBHRX_DEVICE_SELECTION_H_

#include "iree/base/api.h"
#include "iree/hal/driver.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Resolves an ordered device selector against physical HAL device entries.
//
// An empty selector exposes every physical entry in enumeration order. A
// non-empty selector is a comma-separated list of canonical decimal physical
// ordinals or exact device paths. Duplicate entries retain their first
// position. Parsing stops at the first invalid entry, matching visibility-list
// semantics where entries to the right of an invalid selector are hidden.
// Pseudo-device entries with empty paths do not participate in ordinal
// assignment or selection.
iree_status_t hrx_gpu_resolve_device_selection(
    iree_string_view_t selector, iree_host_size_t device_info_count,
    const iree_hal_device_info_t* device_infos,
    iree_host_size_t selected_index_capacity,
    iree_host_size_t* out_selected_index_count,
    iree_host_size_t* out_selected_indices);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // LIBHRX_SRC_LIBHRX_DEVICE_SELECTION_H_
