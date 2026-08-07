// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0

#include "device_selection.h"

#include <stdint.h>

static bool hrx_gpu_parse_canonical_ordinal(iree_string_view_t value,
                                            iree_host_size_t* out_ordinal) {
  if (iree_string_view_is_empty(value)) return false;
  if (value.size > 1 && value.data[0] == '0') return false;

  iree_host_size_t ordinal = 0;
  for (iree_host_size_t i = 0; i < value.size; ++i) {
    const char character = value.data[i];
    if (character < '0' || character > '9') return false;
    const iree_host_size_t digit = (iree_host_size_t)(character - '0');
    if (ordinal > (IREE_HOST_SIZE_MAX - digit) / 10) return false;
    ordinal = ordinal * 10 + digit;
  }

  *out_ordinal = ordinal;
  return true;
}

static bool hrx_gpu_selection_contains(iree_host_size_t selected_index_count,
                                       const iree_host_size_t* selected_indices,
                                       iree_host_size_t index) {
  for (iree_host_size_t i = 0; i < selected_index_count; ++i) {
    if (selected_indices[i] == index) return true;
  }
  return false;
}

static bool hrx_gpu_find_physical_device_by_ordinal(
    iree_host_size_t device_info_count,
    const iree_hal_device_info_t* device_infos, iree_host_size_t ordinal,
    iree_host_size_t* out_index) {
  iree_host_size_t physical_ordinal = 0;
  for (iree_host_size_t i = 0; i < device_info_count; ++i) {
    if (iree_string_view_is_empty(device_infos[i].path)) continue;
    if (physical_ordinal == ordinal) {
      *out_index = i;
      return true;
    }
    ++physical_ordinal;
  }
  return false;
}

static bool hrx_gpu_find_physical_device_by_path(
    iree_host_size_t device_info_count,
    const iree_hal_device_info_t* device_infos, iree_string_view_t path,
    iree_host_size_t* out_index) {
  for (iree_host_size_t i = 0; i < device_info_count; ++i) {
    if (iree_string_view_is_empty(device_infos[i].path)) continue;
    if (iree_string_view_equal_case(path, device_infos[i].path)) {
      *out_index = i;
      return true;
    }
  }
  return false;
}

iree_status_t hrx_gpu_resolve_device_selection(
    iree_string_view_t selector, iree_host_size_t device_info_count,
    const iree_hal_device_info_t* device_infos,
    iree_host_size_t selected_index_capacity,
    iree_host_size_t* out_selected_index_count,
    iree_host_size_t* out_selected_indices) {
  if (!out_selected_index_count || (device_info_count > 0 && !device_infos) ||
      (selected_index_capacity > 0 && !out_selected_indices)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid device selection storage");
  }
  *out_selected_index_count = 0;

  if (iree_string_view_is_empty(selector)) {
    for (iree_host_size_t i = 0;
         i < device_info_count &&
         *out_selected_index_count < selected_index_capacity;
         ++i) {
      if (iree_string_view_is_empty(device_infos[i].path)) continue;
      out_selected_indices[(*out_selected_index_count)++] = i;
    }
    return iree_ok_status();
  }

  while (!iree_string_view_is_empty(selector) &&
         *out_selected_index_count < selected_index_capacity) {
    iree_string_view_t token = iree_string_view_empty();
    iree_string_view_split(selector, ',', &token, &selector);

    iree_host_size_t device_info_index = 0;
    iree_host_size_t ordinal = 0;
    bool found = false;
    if (hrx_gpu_parse_canonical_ordinal(token, &ordinal)) {
      found = hrx_gpu_find_physical_device_by_ordinal(
          device_info_count, device_infos, ordinal, &device_info_index);
    } else {
      found = hrx_gpu_find_physical_device_by_path(
          device_info_count, device_infos, token, &device_info_index);
    }
    if (!found) break;

    if (!hrx_gpu_selection_contains(*out_selected_index_count,
                                    out_selected_indices, device_info_index)) {
      out_selected_indices[(*out_selected_index_count)++] = device_info_index;
    }
  }

  return iree_ok_status();
}
