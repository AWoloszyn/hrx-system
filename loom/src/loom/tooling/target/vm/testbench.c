// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/vm/testbench.h"

#include "iree/vm/bytecode/module.h"
#include "iree/vm/execution.h"
#include "iree/vm/sync.h"
#include "loom/ops/func/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/rewrite/module_projection.h"
#include "loom/target/arch/vm/module.h"
#include "loom/target/arch/vm/provider.h"
#include "loom/tooling/compile/pipeline.h"

void loom_vm_testbench_initialize(
    const loom_target_environment_t* target_environment,
    iree_allocator_t host_allocator, loom_vm_testbench_t* out_testbench) {
  *out_testbench = (loom_vm_testbench_t){
      .target_environment = target_environment,
      .host_allocator = host_allocator,
  };
}

void loom_vm_testbench_deinitialize(loom_vm_testbench_t* testbench) {
  iree_vm_invocation_free(testbench->invocation);
  iree_vm_process_release(testbench->process);
  memset(testbench, 0, sizeof(*testbench));
}

// The compiler copy and all compiler scratch die before the runtime sees the
// image. This exercises the artifact ownership boundary on every test module.
static iree_status_t loom_vm_testbench_compile(loom_vm_testbench_t* testbench,
                                               const loom_module_t* source,
                                               iree_byte_span_t* out_contents) {
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(32 * 1024, testbench->host_allocator, &pool);
  iree_arena_allocator_t arena;
  iree_arena_initialize(&pool, &arena);
  loom_ir_module_projection_t projection = {0};
  loom_module_t* module = NULL;
  const loom_ir_module_clone_options_t clone_options = {0};
  iree_status_t status =
      loom_ir_module_clone(source, &clone_options, &pool, &arena,
                           testbench->host_allocator, &projection, &module);
  const loom_target_profile_t* profile = NULL;
  if (iree_status_is_ok(status)) {
    status = loom_vm_target_provider.select_profile(IREE_SV("core"), &profile);
  }
  loom_target_specialization_request_t* requests = NULL;
  iree_host_size_t request_count = 0;
  if (iree_status_is_ok(status)) {
    status = iree_arena_allocate_array(&arena, module->symbols.count,
                                       sizeof(*requests), (void**)&requests);
  }
  if (iree_status_is_ok(status)) {
    memset(requests, 0, module->symbols.count * sizeof(*requests));
    // Test-library linking internalizes implementation dependencies. Publish
    // only the function-call roots already identified by the shared planner;
    // the compiler still owns their complete transitive closure.
    for (iree_host_size_t i = 0; i < testbench->plan->case_count; ++i) {
      const loom_testbench_case_plan_t* case_plan = &testbench->plan->cases[i];
      for (iree_host_size_t j = 0; j < case_plan->invocation_count; ++j) {
        const loom_testbench_invocation_plan_t* call =
            &case_plan->invocations[j];
        if (call->kind != LOOM_TESTBENCH_INVOCATION_FUNCTION_CALL) continue;
        const loom_func_like_t function = loom_func_like_cast(
            module,
            module->symbols.entries[call->callee_ref.symbol_id].defining_op);
        loom_op_attrs(function.op)[function.vtable->visibility_attr_index] =
            loom_attr_enum(LOOM_FUNC_VISIBILITY_PUBLIC);
        const loom_symbol_t* symbol =
            &module->symbols.entries[call->callee_ref.symbol_id];
        requests[call->callee_ref.symbol_id] =
            (loom_target_specialization_request_t){
                .function_name = module->strings.entries[symbol->name_id],
                .target_profile = profile,
            };
      }
    }
    // Repeated calls and cases share one specialization request. Compact the
    // symbol-indexed roots; the compiler specializes their transitive callees.
    for (loom_symbol_id_t i = 0; i < module->symbols.count; ++i) {
      if (requests[i].target_profile) {
        requests[request_count++] = requests[i];
      }
    }
  }
  loom_target_low_descriptor_registry_t registry = {0};
  if (iree_status_is_ok(status)) {
    status = loom_target_environment_initialize_low_descriptor_registry(
        testbench->target_environment, &registry);
  }
  loom_compile_pipeline_result_t pipeline = {0};
  if (iree_status_is_ok(status)) {
    loom_compile_pipeline_options_t options;
    loom_compile_pipeline_options_initialize(&options);
    options.target_environment = testbench->target_environment;
    options.target_specializations =
        (loom_target_specialization_request_list_t){requests, request_count};
    options.low_descriptor_registry = &registry;
    status = loom_compile_run_pipeline(module, &options, &pool, &pipeline);
    if (iree_status_is_ok(status) && pipeline.pass.error_count) {
      status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "VM compilation failed; see diagnostics");
    }
  }
  loom_target_emit_artifact_t artifact = {0};
  if (iree_status_is_ok(status)) {
    const loom_target_emit_request_t request = {
        .target_environment = testbench->target_environment,
        .low_descriptor_registry = &registry.registry,
        .module = module,
        .function_versions = &pipeline.function_versions.list,
        .scratch_arena = &arena,
        .allocator = testbench->host_allocator,
    };
    status = loom_vm_module_emit(&request, &artifact);
  }
  if (iree_status_is_ok(status)) {
    status = iree_byte_sequence_clone(artifact.contents,
                                      testbench->host_allocator, out_contents);
  }
  loom_target_emit_artifact_release(&artifact);
  loom_compile_pipeline_result_deinitialize(&pipeline);
  loom_module_free(module);
  iree_arena_deinitialize(&arena);
  iree_arena_block_pool_deinitialize(&pool);
  return status;
}

static iree_status_t loom_vm_testbench_prepare(loom_vm_testbench_t* testbench,
                                               const loom_module_t* source) {
  iree_byte_span_t contents = iree_byte_span_empty();
  IREE_RETURN_IF_ERROR(loom_vm_testbench_compile(testbench, source, &contents));
  iree_vm_environment_t* environment = NULL;
  iree_vm_module_t* module = NULL;
  iree_vm_program_t* program = NULL;
  iree_vm_invocation_t* invocation = NULL;
  iree_vm_process_t* process = NULL;
  iree_status_t status =
      iree_vm_environment_allocate(testbench->host_allocator, &environment);
  if (iree_status_is_ok(status)) {
    status = iree_vm_bytecode_module_create(
        environment, IREE_SV("test"),
        (iree_vm_bytecode_module_storage_t){
            .contents =
                iree_make_const_byte_span(contents.data, contents.data_length),
            .deallocator = testbench->host_allocator},
        testbench->host_allocator, &module);
    if (iree_status_is_ok(status)) contents = iree_byte_span_empty();
  }
  iree_allocator_free(testbench->host_allocator, contents.data);
  iree_vm_environment_free(environment);
  if (iree_status_is_ok(status)) {
    status = iree_vm_program_create(
        (iree_vm_program_modules_t){.executable = module},
        testbench->host_allocator, &program);
  }
  iree_vm_module_release(module);
  if (iree_status_is_ok(status)) {
    status = iree_vm_invocation_allocate(16 * 1024, testbench->host_allocator,
                                         &invocation);
  }
  if (iree_status_is_ok(status)) {
    status = iree_vm_process_create(program, invocation,
                                    iree_vm_variant_span_empty(),
                                    testbench->host_allocator, &process);
  }
  iree_vm_program_release(program);
  if (iree_status_is_ok(status)) {
    testbench->invocation = invocation;
    testbench->process = process;
  } else {
    iree_vm_invocation_free(invocation);
    iree_vm_process_release(process);
  }
  return status;
}

static iree_status_t loom_vm_testbench_invoke(
    void* user_data, const loom_testbench_invocation_plan_t* invocation,
    iree_host_size_t workload_count, const loom_testbench_value_t* workloads,
    iree_host_size_t input_count, const loom_testbench_value_t* inputs,
    iree_host_size_t result_count, loom_testbench_value_t* out_results) {
  loom_vm_testbench_t* testbench = user_data;
  if (input_count > IREE_VM_CALL_DIRECT_REGISTER_COUNT ||
      result_count > IREE_VM_CALL_DIRECT_REGISTER_COUNT) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "VM test invocation requires overflow marshalling");
  }
  if (!testbench->process) {
    IREE_RETURN_IF_ERROR(
        loom_vm_testbench_prepare(testbench, invocation->module));
  }
  const loom_symbol_t* symbol =
      &invocation->module->symbols.entries[invocation->callee_ref.symbol_id];
  const loom_func_like_t function =
      loom_func_like_const_cast(invocation->module, symbol->defining_op);
  uint16_t parameter_count = 0;
  const loom_value_id_t* parameter_ids =
      loom_func_like_arg_ids(function, &parameter_count);
  const loom_string_id_t export_name = loom_func_like_export_symbol(function);
  iree_vm_function_t callee = iree_vm_function_null();
  IREE_RETURN_IF_ERROR(iree_vm_process_lookup_function(
      testbench->process, IREE_SV("test"),
      invocation->module->strings
          .entries[export_name == LOOM_STRING_ID_INVALID ? symbol->name_id
                                                         : export_name],
      &callee));
  iree_vm_variant_t arguments[IREE_VM_CALL_DIRECT_REGISTER_COUNT] = {0};
  iree_vm_variant_t results[IREE_VM_CALL_DIRECT_REGISTER_COUNT] = {0};
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < parameter_count && iree_status_is_ok(status);
       ++i) {
    if (inputs[i].kind != LOOM_TESTBENCH_VALUE_KIND_SCALAR) {
      status = iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                                "VM test input requires scalar marshalling");
    } else if (inputs[i].scalar.kind == IREE_TOOLING_VALUE_KIND_I32) {
      const loom_scalar_type_t scalar_type = loom_type_element_type(
          loom_module_value_type(invocation->module, parameter_ids[i]));
      if (scalar_type == LOOM_SCALAR_TYPE_I8) {
        arguments[i] =
            iree_vm_variant_from_i8((int8_t)inputs[i].scalar.storage.i32);
      } else if (scalar_type == LOOM_SCALAR_TYPE_I16) {
        arguments[i] =
            iree_vm_variant_from_i16((int16_t)inputs[i].scalar.storage.i32);
      } else {
        arguments[i] = iree_vm_variant_from_i32(inputs[i].scalar.storage.i32);
      }
    } else if (inputs[i].scalar.kind == IREE_TOOLING_VALUE_KIND_I64) {
      arguments[i] = iree_vm_variant_from_i64(inputs[i].scalar.storage.i64);
    } else if (inputs[i].scalar.kind == IREE_TOOLING_VALUE_KIND_F32) {
      arguments[i] = iree_vm_variant_from_f32(inputs[i].scalar.storage.f32);
    } else if (inputs[i].scalar.kind == IREE_TOOLING_VALUE_KIND_F64) {
      arguments[i] = iree_vm_variant_from_f64(inputs[i].scalar.storage.f64);
    } else {
      status = iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                                "VM test scalar kind %u is not implemented",
                                inputs[i].scalar.kind);
    }
  }
  if (iree_status_is_ok(status)) {
    status =
        iree_vm_invoke(testbench->invocation, callee,
                       iree_vm_variant_span_from_ptr(arguments, input_count),
                       iree_vm_variant_span_from_ptr(results, result_count));
  }
  for (iree_host_size_t i = 0; i < result_count && iree_status_is_ok(status);
       ++i) {
    out_results[i].kind = LOOM_TESTBENCH_VALUE_KIND_SCALAR;
    switch (iree_vm_variant_scalar_type(results[i])) {
      case IREE_VM_SCALAR_TYPE_I8: {
        int8_t value = 0;
        status = iree_vm_i8_from_variant(results[i], &value);
        out_results[i].scalar.kind = IREE_TOOLING_VALUE_KIND_I32;
        out_results[i].scalar.storage.i32 = value;
        break;
      }
      case IREE_VM_SCALAR_TYPE_I16: {
        int16_t value = 0;
        status = iree_vm_i16_from_variant(results[i], &value);
        out_results[i].scalar.kind = IREE_TOOLING_VALUE_KIND_I32;
        out_results[i].scalar.storage.i32 = value;
        break;
      }
      case IREE_VM_SCALAR_TYPE_I32:
        out_results[i].scalar.kind = IREE_TOOLING_VALUE_KIND_I32;
        status = iree_vm_i32_from_variant(results[i],
                                          &out_results[i].scalar.storage.i32);
        break;
      case IREE_VM_SCALAR_TYPE_I64:
        out_results[i].scalar.kind = IREE_TOOLING_VALUE_KIND_I64;
        status = iree_vm_i64_from_variant(results[i],
                                          &out_results[i].scalar.storage.i64);
        break;
      case IREE_VM_SCALAR_TYPE_F32:
        out_results[i].scalar.kind = IREE_TOOLING_VALUE_KIND_F32;
        status = iree_vm_f32_from_variant(results[i],
                                          &out_results[i].scalar.storage.f32);
        break;
      case IREE_VM_SCALAR_TYPE_F64:
        out_results[i].scalar.kind = IREE_TOOLING_VALUE_KIND_F64;
        status = iree_vm_f64_from_variant(results[i],
                                          &out_results[i].scalar.storage.f64);
        break;
      default:
        status = iree_make_status(
            IREE_STATUS_UNIMPLEMENTED,
            "VM test result requires supported scalar marshalling");
        break;
    }
  }
  iree_vm_variant_span_reset(
      iree_vm_variant_span_from_ptr(results, result_count));
  return status;
}

loom_testbench_invocation_provider_t loom_vm_testbench_invocation_provider(
    void* user_data, const loom_testbench_module_plan_t* plan) {
  loom_vm_testbench_t* testbench = user_data;
  testbench->plan = plan;
  return (loom_testbench_invocation_provider_t){
      .invoke = loom_vm_testbench_invoke,
      .user_data = testbench,
  };
}
