// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/loom-check/comparison.h"

static iree_status_t loom_check_match_lines(iree_string_view_t expected,
                                            loom_check_result_t* result) {
  bool has_positive_check = false;
  result->raw_outcome = LOOM_CHECK_PASS;
  iree_host_size_t line_number = 0;
  while (!iree_string_view_is_empty(expected)) {
    iree_string_view_t line;
    iree_string_view_split(expected, '\n', &line, &expected);
    ++line_number;
    line = iree_string_view_trim(line);
    if (iree_string_view_is_empty(line) ||
        iree_string_view_starts_with(line, IREE_SV("//"))) {
      continue;
    }
    bool negative = false;
    if (iree_string_view_consume_prefix(&line, IREE_SV("CHECK:"))) {
      has_positive_check = true;
    } else if (iree_string_view_consume_prefix(&line, IREE_SV("CHECK-NOT:"))) {
      negative = true;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "expected CHECK: or CHECK-NOT: at expected line "
                              "%" PRIhsz,
                              line_number);
    }
    iree_string_view_t pattern = iree_string_view_trim(line);
    if (iree_string_view_is_empty(pattern)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "empty check pattern at expected line %" PRIhsz,
                              line_number);
    }

    bool matched = false;
    iree_string_view_t output =
        iree_string_builder_view(&result->actual_output);
    while (!iree_string_view_is_empty(output)) {
      iree_string_view_t output_line;
      iree_string_view_split(output, '\n', &output_line, &output);
      output_line = iree_string_view_trim(output_line);
      if (!iree_string_view_match_pattern(output_line, pattern)) {
        continue;
      }
      matched = true;
      if (negative) {
        IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
            &result->detail,
            "CHECK-NOT at expected line %" PRIhsz " matched: %.*s\n",
            line_number, (int)output_line.size, output_line.data));
      }
      break;
    }
    if (matched == negative) {
      result->raw_outcome = LOOM_CHECK_FAIL;
      if (!negative) {
        IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
            &result->detail,
            "CHECK at expected line %" PRIhsz " did not match: %.*s\n",
            line_number, (int)pattern.size, pattern.data));
      }
    }
  }
  if (!has_positive_check) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "with-checks requires at least one CHECK: pattern");
  }
  return iree_ok_status();
}

iree_status_t loom_check_compare_output(const loom_test_case_t* test_case,
                                        iree_allocator_t allocator,
                                        loom_check_result_t* result) {
  if (iree_all_bits_set(test_case->output_flags, LOOM_TEST_OUTPUT_CHECKS)) {
    return loom_check_match_lines(test_case->expected, result);
  }

  // Exact goldens ignore standalone comments and outer whitespace. The output
  // printer owns canonical blank-line placement within the comparable text.
  iree_string_builder_t stripped_expected;
  iree_string_builder_initialize(allocator, &stripped_expected);
  iree_status_t status =
      loom_test_file_remove_comments(test_case->expected, &stripped_expected);
  if (iree_status_is_ok(status)) {
    iree_string_view_t actual =
        iree_string_view_trim(iree_string_builder_view(&result->actual_output));
    iree_string_view_t expected =
        iree_string_view_trim(iree_string_builder_view(&stripped_expected));
    if (iree_string_view_equal(actual, expected)) {
      result->raw_outcome = LOOM_CHECK_PASS;
    } else {
      result->raw_outcome = LOOM_CHECK_FAIL;
      status =
          loom_check_result_record_diff(expected, actual, allocator, result);
    }
  }
  iree_string_builder_deinitialize(&stripped_expected);
  return status;
}
