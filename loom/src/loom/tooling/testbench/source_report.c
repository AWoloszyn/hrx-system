// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/testbench/source_report.h"

static const loom_location_entry_t* loom_testbench_file_location(
    const loom_module_t* module, loom_location_id_t location_id) {
  while (location_id != LOOM_LOCATION_UNKNOWN &&
         (iree_host_size_t)location_id < module->locations.count) {
    const loom_location_entry_t* entry =
        &module->locations.entries[location_id];
    switch (entry->kind) {
      case LOOM_LOCATION_FILE:
        return entry;
      case LOOM_LOCATION_TAGGED:
        location_id = entry->tagged.child;
        continue;
      default:
        return NULL;
    }
  }
  return NULL;
}

iree_status_t loom_testbench_write_source_location_json(
    const loom_module_t* module, loom_location_id_t location_id,
    loom_json_object_writer_t* object) {
  const loom_location_entry_t* location =
      loom_testbench_file_location(module, location_id);
  if (location == NULL || location->file.source_id >= module->sources.count) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_json_object_begin_field(object, IREE_SV("source_location")));
  loom_json_object_writer_t location_object;
  IREE_RETURN_IF_ERROR(
      loom_json_object_begin(object->stream, &location_object));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &location_object, IREE_SV("filename"),
      module->sources.entries[location->file.source_id]));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint32_field(
      &location_object, IREE_SV("start_line"), location->file.start_line));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint32_field(
      &location_object, IREE_SV("start_column"), location->file.start_col));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint32_field(
      &location_object, IREE_SV("end_line"), location->file.end_line));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint32_field(
      &location_object, IREE_SV("end_column"), location->file.end_col));
  return loom_json_object_end(&location_object);
}
