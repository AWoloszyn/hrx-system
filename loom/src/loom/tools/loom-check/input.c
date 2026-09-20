// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/loom-check/input.h"

iree_status_t loom_check_load_input(
    const loom_test_case_t* test_case, const loom_input_request_t* request,
    const loom_check_environment_t* environment, loom_context_t* context,
    iree_arena_block_pool_t* block_pool,
    const loom_text_parse_options_t* parse_options,
    iree_allocator_t host_allocator, loom_input_module_t* out_input) {
  *out_input = (loom_input_module_t){0};
  loom_input_request_t case_request = *request;
  case_request.source = test_case->input;
  case_request.parse_options = *parse_options;
  if (test_case->input_options.format.size) {
    case_request.format = test_case->input_options.format;
    case_request.options = test_case->input_options.arguments;
  }
  const loom_input_provider_t* provider = NULL;
  IREE_RETURN_IF_ERROR(loom_input_provider_select(
      environment->input_providers, case_request.format, case_request.path,
      &provider));
  iree_string_builder_t stripped;
  iree_string_builder_initialize(host_allocator, &stripped);
  iree_status_t status = iree_ok_status();
  if (provider == &loom_input_text_provider) {
    status = loom_test_file_strip_comments(case_request.source, &stripped);
    case_request.source = iree_string_builder_view(&stripped);
  }
  if (iree_status_is_ok(status)) {
    status = loom_input_module_load(provider, &case_request, context,
                                    block_pool, host_allocator, out_input);
  }
  iree_string_builder_deinitialize(&stripped);
  return status;
}
