// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/reporting/config_bindings.h"

#include <string.h>

// Each block owns one row and its variable-length strings. Ordinary row-list
// destruction frees them together; copying bindings preserves that ownership.
static iree_status_t loom_target_compile_report_config_bindings_append(
    loom_target_compile_report_row_list_t* list,
    const loom_target_compile_report_config_binding_row_t* row,
    iree_allocator_t allocator) {
  if (iree_allocator_is_null(allocator)) {
    return iree_ok_status();
  }
  iree_host_size_t allocation_size =
      sizeof(loom_target_compile_report_vec_t) + sizeof(*row);
  if (!iree_host_size_checked_add(allocation_size, row->key.size,
                                  &allocation_size) ||
      !iree_host_size_checked_add(allocation_size, row->value.size,
                                  &allocation_size)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "compile report config binding is too large");
  }
  loom_target_compile_report_vec_t* block = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, allocation_size, (void**)&block));
  *block = (loom_target_compile_report_vec_t){.count = 1, .capacity = 1};
  loom_target_compile_report_config_binding_row_t* owned_row =
      (loom_target_compile_report_config_binding_row_t*)(block + 1);
  char* storage = (char*)(owned_row + 1);
  memcpy(storage, row->key.data, row->key.size);
  memcpy(storage + row->key.size, row->value.data, row->value.size);
  *owned_row = (loom_target_compile_report_config_binding_row_t){
      .key = iree_make_string_view(storage, row->key.size),
      .value = iree_make_string_view(storage + row->key.size, row->value.size),
  };
  if (list->tail) {
    list->tail->next = block;
  } else {
    list->head = block;
  }
  list->tail = block;
  ++list->count;
  return iree_ok_status();
}

iree_status_t loom_target_compile_report_config_bindings_append_all(
    loom_target_compile_report_row_list_t* target,
    const loom_target_compile_report_row_list_t* source,
    iree_allocator_t allocator) {
  for (const loom_target_compile_report_vec_t* block = source->head;
       block != NULL; block = block->next) {
    const loom_target_compile_report_config_binding_row_t* rows =
        (const loom_target_compile_report_config_binding_row_t*)
            loom_target_compile_report_vec_const_rows(block);
    for (iree_host_size_t i = 0; i < block->count; ++i) {
      IREE_RETURN_IF_ERROR(loom_target_compile_report_config_bindings_append(
          target, &rows[i], allocator));
    }
  }
  return iree_ok_status();
}

iree_status_t loom_target_compile_report_record_config_binding_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_config_binding_row_t* row) {
  report->detail_flags |= LOOM_TARGET_COMPILE_REPORT_DETAIL_CONFIG_BINDING_ROWS;
  if (!loom_target_compile_report_wants_details(
          report, LOOM_TARGET_COMPILE_REPORT_DETAIL_CONFIG_BINDING_ROWS)) {
    return iree_ok_status();
  }
  return loom_target_compile_report_config_bindings_append(
      &report->config_binding_rows, row, report->allocator);
}
