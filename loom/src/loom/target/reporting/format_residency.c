// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <inttypes.h>

#include "loom/target/reporting/format_json.h"
#include "loom/target/reporting/format_text.h"

static iree_status_t loom_target_compile_report_format_residency_row_json(
    const loom_target_compile_report_residency_constraint_row_t* row,
    iree_host_size_t index, loom_output_stream_t* stream) {
  const loom_target_residency_constraint_t* constraint = &row->constraint;
  loom_json_object_writer_t object;
  IREE_RETURN_IF_ERROR(loom_json_object_begin(stream, &object));
  IREE_RETURN_IF_ERROR(
      loom_json_object_write_host_size_field(&object, IREE_SV("index"), index));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("function"), row->function_name));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("name"), constraint->name));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("kind"),
      loom_target_compile_report_residency_constraint_kind_name(
          constraint->kind)));
  if (constraint->kind != LOOM_TARGET_RESIDENCY_CONSTRAINT_FIXED_LIMIT) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("unit"), constraint->unit));
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("allocation_scope"), constraint->allocation_scope));
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("pool_scope"), constraint->pool_scope));
    IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
        &object, IREE_SV("pool_units"), constraint->pool_units));
    IREE_RETURN_IF_ERROR(loom_json_object_write_uint32_field(
        &object, IREE_SV("allocation_granularity"),
        constraint->allocation_granularity));
    if (iree_any_bit_set(constraint->flags,
                         LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_USAGE)) {
      IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
          &object, IREE_SV("units"), constraint->units));
      IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
          &object, IREE_SV("rounded_units"), constraint->rounded_units));
    }
  }
  if (iree_any_bit_set(constraint->flags,
                       LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_TIER)) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_uint32_field(
        &object, IREE_SV("independent_tier"), constraint->tier));
  }
  if (iree_any_bit_set(
          constraint->flags,
          LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_LIMITING_RELATION)) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_bool_field(
        &object, IREE_SV("limiting"),
        iree_any_bit_set(constraint->flags,
                         LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_LIMITING)));
  }
  if (iree_any_bit_set(constraint->flags,
                       LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_REDUCTION)) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
        &object, IREE_SV("reduction_units_to_next_better_tier"),
        constraint->reduction_units_to_next_better_tier));
  }
  return loom_json_object_end(&object);
}

iree_status_t loom_target_compile_report_format_residency_constraints_json(
    const loom_target_compile_report_t* report, loom_output_stream_t* stream) {
  loom_json_object_writer_t object;
  IREE_RETURN_IF_ERROR(loom_json_object_begin(stream, &object));
  IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
      &object, IREE_SV("count"), report->residency_constraint_rows.count));
  IREE_RETURN_IF_ERROR(loom_json_object_begin_field(&object, IREE_SV("rows")));
  loom_json_array_writer_t array;
  IREE_RETURN_IF_ERROR(loom_json_array_begin(stream, &array));
  iree_status_t status = iree_ok_status();
  iree_host_size_t index = 0;
  for (const loom_target_compile_report_vec_t* vec =
           report->residency_constraint_rows.head;
       vec != NULL && iree_status_is_ok(status); vec = vec->next) {
    const loom_target_compile_report_residency_constraint_row_t* rows =
        (const loom_target_compile_report_residency_constraint_row_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count && iree_status_is_ok(status);
         ++i, ++index) {
      status = loom_json_array_begin_element(&array);
      if (iree_status_is_ok(status)) {
        status = loom_target_compile_report_format_residency_row_json(
            &rows[i], index, stream);
      }
    }
  }
  IREE_RETURN_IF_ERROR(status);
  IREE_RETURN_IF_ERROR(loom_json_array_end(&array));
  return loom_json_object_end(&object);
}

static iree_status_t loom_target_compile_report_format_residency_row_text(
    const loom_target_compile_report_residency_constraint_row_t* row,
    iree_string_builder_t* builder) {
  const loom_target_residency_constraint_t* constraint = &row->constraint;
  const iree_string_view_t kind =
      loom_target_compile_report_residency_constraint_kind_name(
          constraint->kind);
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder,
      "COMPILE-REPORT: residency_constraint function=%.*s resource=%.*s "
      "kind=%.*s",
      (int)row->function_name.size, row->function_name.data,
      (int)constraint->name.size, constraint->name.data, (int)kind.size,
      kind.data));
  if (constraint->kind != LOOM_TARGET_RESIDENCY_CONSTRAINT_FIXED_LIMIT) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder,
        " unit=%.*s allocation_scope=%.*s granularity=%" PRIu32
        " pool_units=%" PRIu64 " pool_scope=%.*s",
        (int)constraint->unit.size, constraint->unit.data,
        (int)constraint->allocation_scope.size,
        constraint->allocation_scope.data, constraint->allocation_granularity,
        constraint->pool_units, (int)constraint->pool_scope.size,
        constraint->pool_scope.data));
    if (iree_any_bit_set(constraint->flags,
                         LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_USAGE)) {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          builder, " units=%" PRIu64 " rounded_units=%" PRIu64,
          constraint->units, constraint->rounded_units));
    } else {
      IREE_RETURN_IF_ERROR(
          iree_string_builder_append_cstring(builder, " units=unavailable"));
    }
  }
  if (iree_any_bit_set(constraint->flags,
                       LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_TIER)) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder, " independent_tier=%" PRIu32, constraint->tier));
  } else {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(
        builder, " independent_tier=unavailable"));
  }
  const char* relation = "unknown";
  if (iree_any_bit_set(
          constraint->flags,
          LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_LIMITING_RELATION)) {
    relation = iree_any_bit_set(constraint->flags,
                                LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_LIMITING)
                   ? "true"
                   : "false";
  }
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_format(builder, " limiting=%s", relation));
  if (iree_any_bit_set(constraint->flags,
                       LOOM_TARGET_RESIDENCY_CONSTRAINT_FLAG_HAS_REDUCTION)) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder, " reduction_units_to_next_better_tier=%" PRIu64,
        constraint->reduction_units_to_next_better_tier));
  }
  return iree_string_builder_append_cstring(builder, "\n");
}

iree_status_t loom_target_compile_report_format_residency_constraints_text(
    const loom_target_compile_report_t* report,
    iree_string_builder_t* builder) {
  iree_status_t status = iree_ok_status();
  for (const loom_target_compile_report_vec_t* vec =
           report->residency_constraint_rows.head;
       vec != NULL && iree_status_is_ok(status); vec = vec->next) {
    const loom_target_compile_report_residency_constraint_row_t* rows =
        (const loom_target_compile_report_residency_constraint_row_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count && iree_status_is_ok(status);
         ++i) {
      status = loom_target_compile_report_format_residency_row_text(&rows[i],
                                                                    builder);
    }
  }
  return status;
}
