// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/module_emitter.h"

#include "loom/analysis/symbol_facts.h"
#include "loom/codegen/low/diagnostics.h"
#include "loom/codegen/low/function.h"
#include "loom/error/error_catalog.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/llvmir/descriptors/descriptors.h"
#include "loom/target/arch/llvmir/facts.h"
#include "loom/target/emit/llvmir/function_emitter.h"
#include "loom/target/launch.h"

#define LOOM_LLVMIR_GENERIC_CORE_DESCRIPTOR_SET_KEY \
  IREE_SV("llvmir.generic.core")

typedef struct loom_llvmir_emit_module_state_t {
  // Module containing the emitted low functions.
  loom_module_t* module;
  // Low descriptor registry used to resolve target-bound packets.
  const loom_low_descriptor_registry_t* descriptor_registry;
  // Structured diagnostic emitter for user IR failures.
  iree_diagnostic_emitter_t diagnostic_emitter;
  // Scratch arena borrowed from the emission caller.
  iree_arena_allocator_t* scratch_arena;
  // Function versions observed against the immutable module symbol table.
  loom_target_function_version_snapshot_t function_versions;
  // Optional registry of linked target profiles for kernel projection.
  const loom_llvmir_target_profile_registry_t* target_profile_registry;
  // Cached symbol facts shared by target resolution for every function.
  loom_symbol_fact_table_t symbol_facts;
  // Structured LLVMIR module being built.
  loom_llvmir_module_t* llvmir_module;
  // Number of low functions emitted into |llvmir_module|.
  iree_host_size_t function_count;
  // Number of error diagnostics emitted during semantic emission.
  iree_host_size_t error_count;
} loom_llvmir_emit_module_state_t;

static iree_status_t loom_llvmir_emit_descriptor_set_diagnostic(
    loom_llvmir_emit_module_state_t* state, const loom_op_t* op,
    iree_string_view_t function_name,
    const loom_low_resolved_target_t* target) {
  const iree_string_view_t descriptor_set_key = loom_low_descriptor_set_string(
      target->descriptor_set, target->descriptor_set->key_string_offset);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(function_name),
      loom_param_string(descriptor_set_key),
      loom_param_string(LOOM_LLVMIR_LOW_EMITTER_KEY),
      loom_param_string(LOOM_LLVMIR_GENERIC_CORE_DESCRIPTOR_SET_KEY),
  };
  const loom_diagnostic_emission_t emission = {
      .op = op,
      .error = LOOM_ERR_TARGET_055,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  IREE_RETURN_IF_ERROR(
      iree_diagnostic_emit(state->diagnostic_emitter, &emission));
  ++state->error_count;
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_projection_diagnostic(
    loom_llvmir_emit_function_state_t* state) {
  const loom_target_bundle_t* bundle =
      loom_low_resolved_target_bundle(state->target);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->target->target_name),
      loom_param_string(bundle->export_plan->name),
      loom_param_string(bundle->config->name),
      loom_param_string(LOOM_LLVMIR_LOW_EMITTER_KEY),
      loom_param_string(
          loom_target_codegen_format_name(bundle->snapshot->codegen_format)),
      loom_param_string(
          loom_target_abi_kind_name(bundle->export_plan->abi_kind)),
  };
  return loom_llvmir_emit_diagnostic(state, state->function_op,
                                     LOOM_ERR_TARGET_036, params,
                                     IREE_ARRAYSIZE(params));
}

static iree_status_t loom_llvmir_emit_no_functions_diagnostic(
    loom_llvmir_emit_module_state_t* state) {
  const loom_diagnostic_param_t params[] = {
      loom_param_string(LOOM_LLVMIR_LOW_EMITTER_KEY),
  };
  const loom_diagnostic_emission_t emission = {
      .error = LOOM_ERR_TARGET_011,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  IREE_RETURN_IF_ERROR(
      iree_diagnostic_emit(state->diagnostic_emitter, &emission));
  ++state->error_count;
  return iree_ok_status();
}

static uint32_t loom_llvmir_emit_workgroup_size_dimension_count(
    const loom_target_workgroup_size_t* size) {
  return (size->x != 0 ? 1u : 0u) + (size->y != 0 ? 1u : 0u) +
         (size->z != 0 ? 1u : 0u);
}

static iree_status_t loom_llvmir_emit_prepare_function_profile(
    loom_llvmir_emit_function_state_t* state,
    const loom_llvmir_target_profile_registry_t* target_profile_registry) {
  const loom_target_bundle_t* bundle =
      loom_low_resolved_target_bundle(state->target);
  const loom_target_snapshot_t* snapshot = bundle->snapshot;
  const loom_target_export_plan_t* export_plan = bundle->export_plan;
  iree_string_view_t target_triple = iree_string_view_empty();
  iree_string_view_t data_layout = iree_string_view_empty();
  iree_string_view_t target_cpu = iree_string_view_empty();
  iree_string_view_t target_features = iree_string_view_empty();
  const loom_llvmir_target_facts_t* target_facts =
      loom_llvmir_target_facts_cast(state->target->target_facts);
  if (target_facts != NULL) {
    target_triple = target_facts->target_triple;
    data_layout = target_facts->data_layout;
    target_cpu = target_facts->target_cpu;
    target_features = target_facts->target_features;
  }

  loom_llvmir_target_env_t projected_env = {
      .name = snapshot->name,
      .target_triple = target_triple,
      .data_layout = data_layout,
      .object_format = LOOM_LLVMIR_OBJECT_FORMAT_UNKNOWN,
      .default_pointer_bitwidth = snapshot->default_pointer_bitwidth,
      .index_bitwidth = snapshot->index_bitwidth,
      .offset_bitwidth = snapshot->offset_bitwidth,
      .address_spaces =
          {
              .generic = snapshot->memory_spaces.generic,
              .global = snapshot->memory_spaces.global,
              .local = snapshot->memory_spaces.workgroup,
              .constant = snapshot->memory_spaces.constant,
              .private_memory = snapshot->memory_spaces.private_memory,
              .buffer_resource = snapshot->memory_spaces.descriptor,
          },
  };
  loom_llvmir_target_profile_t projected_profile = {
      .name = export_plan->name,
      .target_env = &projected_env,
      .kind = LOOM_LLVMIR_TARGET_PROFILE_HOST_OBJECT,
      .target_cpu = target_cpu,
      .target_features = target_features,
  };

  switch (export_plan->abi_kind) {
    case LOOM_TARGET_ABI_OBJECT_FUNCTION:
      break;
    case LOOM_TARGET_ABI_HAL_KERNEL: {
      const loom_target_workgroup_size_t* required_workgroup_size =
          &export_plan->hal_kernel.required_workgroup_size;
      if (!loom_target_workgroup_size_is_concrete(required_workgroup_size)) {
        return loom_llvmir_emit_shape_diagnostic(
            state, state->function_op, IREE_SV("hal_kernel_workgroup_size"),
            loom_llvmir_emit_workgroup_size_dimension_count(
                required_workgroup_size),
            3);
      }
      const loom_llvmir_target_profile_t* provider_profile = NULL;
      const loom_llvmir_target_profile_projection_request_t request = {
          .bundle = bundle,
          .target_triple = target_triple,
      };
      if (!loom_llvmir_target_profile_registry_project_bundle(
              target_profile_registry, &request, &provider_profile)) {
        return loom_llvmir_emit_projection_diagnostic(state);
      }
      projected_env = *provider_profile->target_env;
      if (!iree_string_view_is_empty(target_triple)) {
        projected_env.target_triple = target_triple;
      }
      if (!iree_string_view_is_empty(data_layout)) {
        projected_env.data_layout = data_layout;
      }
      projected_profile = *provider_profile;
      projected_profile.target_env = &projected_env;
      if (!iree_string_view_is_empty(target_cpu)) {
        projected_profile.target_cpu = target_cpu;
      }
      if (!iree_string_view_is_empty(target_features)) {
        projected_profile.target_features = target_features;
      }
      break;
    }
    default:
      return loom_llvmir_emit_projection_diagnostic(state);
  }

  loom_llvmir_target_profile_storage_initialize_from_bundle(
      bundle, &projected_profile, &state->target_profile_storage);
  state->target_profile = &state->target_profile_storage.profile;
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_prepare_module(
    loom_llvmir_emit_module_state_t* state,
    const loom_llvmir_target_profile_t* profile, iree_allocator_t allocator) {
  if (state->llvmir_module != NULL) {
    return iree_ok_status();
  }

  loom_llvmir_target_config_t config = {0};
  loom_llvmir_target_profile_module_config(profile, iree_string_view_empty(),
                                           &config);
  config.producer = IREE_SV("loom");
  return loom_llvmir_module_allocate(&config, allocator, &state->llvmir_module);
}

static iree_status_t loom_llvmir_emit_low_function_into_module(
    loom_llvmir_emit_module_state_t* module_state, loom_op_t* low_function_op,
    iree_allocator_t allocator) {
  const loom_func_like_t function =
      loom_func_like_cast(module_state->module, low_function_op);
  const loom_symbol_ref_t function_ref = loom_func_like_callee(function);
  const loom_target_function_version_t* function_version =
      loom_target_function_version_snapshot_at(&module_state->function_versions,
                                               function_ref.symbol_id);
  loom_low_resolved_target_t target = {0};
  IREE_RETURN_IF_ERROR(loom_low_resolve_function_target(
      module_state->module, &module_state->symbol_facts, low_function_op,
      function_version ? function_version->function_target_facts : NULL,
      module_state->descriptor_registry, module_state->diagnostic_emitter,
      &target));
  // Concrete targets select their artifact backend; targetless assembly is
  // selected by its representation contract below.
  const loom_target_bundle_t* bundle = loom_low_resolved_target_bundle(&target);
  if (bundle &&
      bundle->snapshot->codegen_format != LOOM_TARGET_CODEGEN_FORMAT_LLVMIR) {
    return iree_ok_status();
  }
  if (target.descriptor_set == NULL) {
    ++module_state->error_count;
    return iree_ok_status();
  }
  if (target.descriptor_set->stable_id !=
      LLVMIR_GENERIC_CORE_DESCRIPTOR_SET_ID) {
    return loom_llvmir_emit_descriptor_set_diagnostic(
        module_state, low_function_op,
        loom_low_diagnostic_function_name(module_state->module,
                                          low_function_op),
        &target);
  }

  loom_llvmir_emit_function_state_t function_state = {
      .diagnostic_emitter = module_state->diagnostic_emitter,
      .module = module_state->module,
      .function_op = low_function_op,
      .body = loom_low_function_const_body(low_function_op),
      .target = &target,
      .function_name = loom_low_diagnostic_function_name(module_state->module,
                                                         low_function_op),
      .scratch_arena = module_state->scratch_arena,
  };
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_function_profile(
      &function_state, module_state->target_profile_registry));
  if (function_state.error_count != 0) {
    module_state->error_count += function_state.error_count;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_module(
      module_state, function_state.target_profile, allocator));
  function_state.llvmir_module = module_state->llvmir_module;
  iree_status_t status = loom_llvmir_emit_function(&function_state);
  module_state->error_count += function_state.error_count;
  if (iree_status_is_ok(status) && module_state->error_count == 0) {
    ++module_state->function_count;
  }
  return status;
}

static iree_status_t loom_llvmir_emit_low_module_options_validate(
    const loom_llvmir_emit_low_module_options_t* options) {
  if (options == NULL || options->entry_count == 0) {
    return iree_ok_status();
  }
  if (options->entry_ops == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "selected LLVMIR low module entries require an entry op list");
  }
  for (iree_host_size_t i = 0; i < options->entry_count; ++i) {
    if (options->entry_ops[i] == NULL) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "selected LLVMIR low module entry list contains a null op");
    }
  }
  return iree_ok_status();
}

static bool loom_llvmir_emit_low_module_options_selects_entry(
    const loom_llvmir_emit_low_module_options_t* options,
    loom_op_t* low_function_op) {
  if (options == NULL || options->entry_count == 0) {
    return true;
  }
  for (iree_host_size_t i = 0; i < options->entry_count; ++i) {
    if (options->entry_ops[i] == low_function_op) {
      return true;
    }
  }
  return false;
}

void loom_llvmir_emit_low_module_options_initialize(
    loom_llvmir_emit_low_module_options_t* out_options) {
  IREE_ASSERT_ARGUMENT(out_options);
  *out_options = (loom_llvmir_emit_low_module_options_t){0};
}

iree_status_t loom_llvmir_emit_low_module(
    loom_module_t* module,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter,
    iree_arena_allocator_t* scratch_arena,
    const loom_llvmir_emit_low_module_options_t* options,
    loom_llvmir_module_t** out_module, iree_allocator_t allocator) {
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(descriptor_registry);
  IREE_ASSERT_ARGUMENT(scratch_arena);
  IREE_ASSERT_ARGUMENT(out_module);
  *out_module = NULL;

  IREE_RETURN_IF_ERROR(loom_llvmir_emit_low_module_options_validate(options));
  loom_llvmir_emit_module_state_t state = {
      .module = module,
      .descriptor_registry = descriptor_registry,
      .diagnostic_emitter = diagnostic_emitter,
      .scratch_arena = scratch_arena,
      .target_profile_registry =
          options ? options->target_profile_registry : NULL,
  };
  loom_symbol_fact_table_initialize(&state.symbol_facts, scratch_arena);
  IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
      module, options ? options->function_versions : NULL, scratch_arena,
      &state.function_versions));

  iree_status_t status = iree_ok_status();
  loom_op_t* op = NULL;
  loom_block_for_each_op(loom_module_block(module), op) {
    if (!loom_low_function_def_isa(op) ||
        !loom_llvmir_emit_low_module_options_selects_entry(options, op)) {
      continue;
    }
    status = loom_llvmir_emit_low_function_into_module(&state, op, allocator);
    if (!iree_status_is_ok(status) || state.error_count != 0) {
      break;
    }
  }

  if (iree_status_is_ok(status) && state.error_count == 0 &&
      state.function_count == 0) {
    status = loom_llvmir_emit_no_functions_diagnostic(&state);
  }
  if (iree_status_is_ok(status) && state.error_count == 0) {
    *out_module = state.llvmir_module;
    state.llvmir_module = NULL;
  }
  loom_llvmir_module_free(state.llvmir_module);
  return status;
}
