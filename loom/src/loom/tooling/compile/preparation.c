// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/preparation.h"

#include "loom/link/linker.h"
#include "loom/ops/op_defs.h"
#include "loom/target/entry_selection.h"
#include "loom/target/module_specialization.h"

static iree_status_t loom_compile_materialize_roots(
    const loom_compile_request_t* request, iree_arena_block_pool_t* block_pool,
    iree_allocator_t allocator, loom_module_t** inout_module) {
  loom_module_t* module = *inout_module;
  iree_string_view_list_t roots = request->roots;
  iree_string_view_t* implicit_root_values = NULL;
  if (roots.count == 0 && request->product == LOOM_COMPILE_PRODUCT_KERNEL) {
    for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
      if (loom_compile_request_symbol_is_implicit_root(
              module, request->product, &module->symbols.entries[i])) {
        ++roots.count;
      }
    }
    if (roots.count == 0) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "kernel product requires a kernel entry or a public or retained "
          "kernel-scoped pipeline or array program root");
    }
    IREE_RETURN_IF_ERROR(iree_allocator_malloc_array(
        allocator, roots.count, sizeof(*implicit_root_values),
        (void**)&implicit_root_values));
    iree_host_size_t root_ordinal = 0;
    for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
      const loom_symbol_t* symbol = &module->symbols.entries[i];
      if (loom_compile_request_symbol_is_implicit_root(module, request->product,
                                                       symbol)) {
        implicit_root_values[root_ordinal++] =
            loom_string_table_get(&module->strings, symbol->name_id);
      }
    }
    roots.values = implicit_root_values;
  }
  if (roots.count == 0) {
    return iree_ok_status();
  }

  const loom_module_t* const source_modules[] = {module};
  iree_string_view_t module_name = iree_string_view_empty();
  if (module->name_id < module->strings.count) {
    module_name = loom_string_table_get(&module->strings, module->name_id);
  }
  loom_module_t* linked_module = NULL;
  iree_status_t status = loom_link_materialized_modules(
      source_modules, IREE_ARRAYSIZE(source_modules),
      &(loom_link_options_t){
          .module_name = module_name,
          .root_symbols = roots,
      },
      block_pool, allocator, &linked_module);
  iree_allocator_free(allocator, implicit_root_values);
  if (iree_status_is_ok(status)) {
    loom_module_free(module);
    *inout_module = linked_module;
  }
  return status;
}

iree_status_t loom_compile_materialize_request(
    const loom_compile_request_t* request,
    const loom_compile_pipeline_options_t* options,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_module_t** inout_module, uint32_t* out_error_count) {
  *out_error_count = 0;
  if (request->product == LOOM_COMPILE_PRODUCT_KERNEL &&
      request->explicit_target.target_profile != NULL) {
    const loom_target_entry_options_t diagnostic_options = {
        .diagnostic_sink = options->diagnostic_sink,
        .source_resolver = options->source_resolver,
        .max_errors = options->max_errors,
    };
    loom_target_entry_diagnostic_emitter_t diagnostic_emitter;
    loom_target_entry_diagnostic_emitter_initialize(
        *inout_module, &diagnostic_options, LOOM_EMITTER_PASS,
        &diagnostic_emitter);
    IREE_RETURN_IF_ERROR(loom_target_specialize_module_kernel_entries(
        options->target_environment, request->explicit_target.target_profile,
        loom_target_entry_emitter(&diagnostic_emitter), block_pool, allocator,
        inout_module, out_error_count));
  }
  if (*out_error_count != 0) {
    return iree_ok_status();
  }
  return loom_compile_materialize_roots(request, block_pool, allocator,
                                        inout_module);
}

iree_status_t loom_compile_run_request_pipeline(
    const loom_compile_request_t* request, loom_module_t* module,
    const loom_compile_pipeline_options_t* options,
    iree_arena_block_pool_t* block_pool,
    loom_compile_pipeline_result_t* out_result) {
  memset(out_result, 0, sizeof(*out_result));
  loom_compile_pipeline_options_t pipeline_options = *options;

  // Selected roots are public or retained by module linking. Specialization
  // owns their transitive callees and carries facts directly through emission,
  // without projecting target definitions back into the authored module.
  iree_arena_allocator_t arena;
  iree_arena_initialize(block_pool, &arena);
  iree_status_t status = iree_ok_status();
  if (request->product == LOOM_COMPILE_PRODUCT_MODULE &&
      request->explicit_target.target_profile != NULL) {
    loom_target_specialization_request_t* specializations = NULL;
    status = iree_arena_allocate_array(&arena, module->symbols.count,
                                       sizeof(*specializations),
                                       (void**)&specializations);
    if (iree_status_is_ok(status)) {
      iree_host_size_t count = 0;
      for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
        const loom_symbol_t* symbol = &module->symbols.entries[i];
        const loom_func_like_t function =
            loom_func_like_cast(module, symbol->defining_op);
        if (loom_func_like_body(function) == NULL ||
            (loom_func_like_is_module_internal(function) &&
             !iree_any_bit_set(symbol->flags, LOOM_SYMBOL_FLAG_RETAIN))) {
          continue;
        }
        specializations[count++] = (loom_target_specialization_request_t){
            .function_name =
                loom_string_table_get(&module->strings, symbol->name_id),
            .target_profile = request->explicit_target.target_profile,
        };
      }
      pipeline_options.target_specializations =
          (loom_target_specialization_request_list_t){specializations, count};
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_compile_run_pipeline(module, &pipeline_options, block_pool,
                                       out_result);
  }
  iree_arena_deinitialize(&arena);
  return status;
}
