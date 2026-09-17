// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/check.h"

#include "iree/vm/bytecode/disassembler.h"
#include "loom/target/arch/vm/module.h"
#include "loom/target/arch/vm/provider.h"
#include "loom/tools/loom-check/diagnostics.h"
#include "loom/tools/loom-check/execute.h"

static bool loom_vm_check_match(const loom_check_emit_provider_t* provider,
                                iree_string_view_t target_name) {
  return iree_string_view_equal(target_name, IREE_SV("vm-dis"));
}

static iree_status_t loom_vm_check_write(void* user_data,
                                         iree_string_view_t fragment) {
  return iree_string_builder_append_string(user_data, fragment);
}

static iree_status_t loom_vm_check_emit(
    const loom_check_emit_provider_t* provider,
    const loom_check_emit_provider_request_t* request) {
  if (!iree_string_view_is_empty(request->target_options)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "vm-dis does not accept options");
  }
  loom_check_prepare_source_low_options_t prepare_options;
  loom_check_prepare_source_low_options_initialize(&prepare_options);
  IREE_RETURN_IF_ERROR(loom_check_prepare_source_low_module(
      request->module, &prepare_options, request->low_registry,
      request->environment, request->source_resolver,
      request->diagnostic_collector, request->block_pool));
  if (request->diagnostic_collector->count) {
    return iree_ok_status();
  }

  loom_check_diagnostic_emitter_capture_t capture = {
      .diagnostic_collector = request->diagnostic_collector,
      .module = request->module,
      .source_resolver = request->source_resolver,
      .emitter = LOOM_EMITTER_PASS,
  };
  const loom_target_emit_request_t emit_request = {
      .low_descriptor_registry = &request->low_registry->registry,
      .module = request->module,
      .diagnostic_emitter = {.fn = loom_check_diagnostic_emitter_capture_emit,
                             .user_data = &capture},
      .scratch_arena = request->case_arena,
      .allocator = request->host_allocator,
  };
  loom_target_emit_artifact_t artifact = {0};
  iree_status_t status = loom_vm_module_emit(&emit_request, &artifact);
  iree_byte_span_t contents = iree_byte_span_empty();
  if (iree_status_is_ok(status)) {
    status = iree_byte_sequence_clone(artifact.contents,
                                      request->host_allocator, &contents);
  }
  loom_target_emit_artifact_release(&artifact);
  if (iree_status_is_ok(status)) {
    status = iree_vm_bytecode_disassemble_module(
        iree_make_const_byte_span(contents.data, contents.data_length),
        (iree_vm_bytecode_disassembler_write_callback_t){
            .fn = loom_vm_check_write,
            .user_data = &request->result->actual_output},
        request->host_allocator);
  }
  iree_allocator_free(request->host_allocator, contents.data);
  return status;
}

static iree_status_t loom_vm_check_append_names(
    const loom_check_emit_provider_t* provider,
    iree_string_builder_t* builder) {
  return iree_string_builder_append_cstring(builder, "vm-dis");
}

static const loom_check_emit_provider_t loom_vm_check_emit_provider = {
    .name = IREE_SVL("vm"),
    .match = loom_vm_check_match,
    .execute = loom_vm_check_emit,
    .append_names = loom_vm_check_append_names,
};

static const loom_check_emit_provider_t* const loom_vm_check_emit_providers[] =
    {
        &loom_vm_check_emit_provider,
};

const loom_check_provider_t loom_vm_check_provider = {
    .name = IREE_SVL("vm"),
    .target_provider = &loom_vm_target_provider,
    .emit_providers = loom_vm_check_emit_providers,
    .emit_provider_count = IREE_ARRAYSIZE(loom_vm_check_emit_providers),
};
