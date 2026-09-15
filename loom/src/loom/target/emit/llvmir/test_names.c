// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/test_names.h"

#include "loom/target/emit/llvmir/builder.h"

static iree_status_t loom_llvmir_test_populate_local_names(
    loom_llvmir_module_t* module) {
  loom_llvmir_type_id_t i32_type;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_module_get_integer_type(module, 32, &i32_type));
  loom_llvmir_function_t* function = NULL;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_function(
      module,
      &(loom_llvmir_function_desc_t){
          .kind = LOOM_LLVMIR_FUNCTION_DEFINITION,
          .name = IREE_SV("local_names"),
          .return_type = i32_type,
          .attr_group_id = LOOM_LLVMIR_ATTR_GROUP_ID_INVALID,
      },
      &function));
  const iree_string_view_t names[] = {
      IREE_SV("entry"), IREE_SV("v2"),   IREE_SV(""),
      IREE_SV("same"),  IREE_SV("same"), IREE_SV("quoted \"name\\"),
  };
  loom_llvmir_value_id_t parameters[IREE_ARRAYSIZE(names)];
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(names); ++i) {
    IREE_RETURN_IF_ERROR(loom_llvmir_function_add_parameter(
        function,
        &(loom_llvmir_parameter_desc_t){.type_id = i32_type, .name = names[i]},
        &parameters[i]));
  }
  loom_llvmir_block_t* entry = NULL;
  loom_llvmir_block_t* exit = NULL;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_function_add_block(function, IREE_SV("entry"), &entry));
  IREE_RETURN_IF_ERROR(
      loom_llvmir_function_add_block(function, IREE_SV("entry"), &exit));
  loom_llvmir_value_id_t sum;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_build_binop(entry,
                              &(loom_llvmir_binop_desc_t){
                                  .result_type = i32_type,
                                  .result_name = IREE_SV("same"),
                                  .op = LOOM_LLVMIR_BINOP_ADD,
                                  .lhs = parameters[0],
                                  .rhs = parameters[2],
                              },
                              &sum));
  IREE_RETURN_IF_ERROR(loom_llvmir_build_br(entry, loom_llvmir_block_id(exit)));
  const loom_llvmir_phi_incoming_t incoming = {
      .value = sum, .predecessor = loom_llvmir_block_id(entry)};
  loom_llvmir_value_id_t result;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_build_phi(exit,
                            &(loom_llvmir_phi_desc_t){
                                .result_type = i32_type,
                                .result_name = IREE_SV("entry"),
                                .incoming = &incoming,
                                .incoming_count = 1,
                            },
                            &result));
  return loom_llvmir_build_ret(exit, result);
}

iree_status_t loom_llvmir_test_build_local_names_module(
    iree_allocator_t allocator, loom_llvmir_module_t** out_module) {
  *out_module = NULL;
  loom_llvmir_module_t* module = NULL;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_allocate(NULL, allocator, &module));
  iree_status_t status = loom_llvmir_test_populate_local_names(module);
  if (iree_status_is_ok(status)) {
    *out_module = module;
  } else {
    loom_llvmir_module_free(module);
  }
  return status;
}
