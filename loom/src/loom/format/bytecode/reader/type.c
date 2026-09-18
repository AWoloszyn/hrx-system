// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/bytecode/reader/type.h"

#include <string.h>

#include "loom/error/error_catalog.h"
#include "loom/format/bytecode/reader/attribute.h"
#include "loom/format/bytecode/reader/module_view.h"

static loom_bytecode_attribute_materializer_t
loom_bytecode_type_attribute_materializer(
    loom_bytecode_type_materializer_t* materializer) {
  return (loom_bytecode_attribute_materializer_t){
      .decoder = materializer->decoder,
      .context = materializer->context,
      .module_view = materializer->module_view,
      .scratch_arena = materializer->scratch_arena,
      .output_module = materializer->output_module,
  };
}

static iree_status_t loom_bytecode_type_materialize_parameterized(
    loom_bytecode_type_materializer_t* materializer,
    const loom_bytecode_parameterized_type_fact_t* fact, loom_type_t* out_type,
    loom_type_id_t* out_type_id) {
  const loom_parameterized_type_descriptor_t* descriptor = fact->descriptor;
  loom_attribute_t* parameters = NULL;
  if (descriptor->parameter_count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        materializer->scratch_arena, descriptor->parameter_count,
        sizeof(*parameters), (void**)&parameters));
    memset(parameters, 0, descriptor->parameter_count * sizeof(*parameters));
  }

  loom_bytecode_reader_cursor_t cursor;
  loom_bytecode_reader_cursor_initialize(
      materializer->bytecode.data + (iree_host_size_t)fact->parameters_offset,
      fact->parameters_length, fact->parameters_offset, IREE_SV("TYPES"),
      &cursor);
  loom_bytecode_attribute_materializer_t attribute_materializer =
      loom_bytecode_type_attribute_materializer(materializer);
  for (uint8_t i = 0; i < fact->present_count; ++i) {
    uint64_t unused_parameter_name_id = 0;
    IREE_RETURN_IF_ERROR(loom_bytecode_reader_read_uvarint(
        materializer->decoder, &cursor, &unused_parameter_name_id));
    const uint8_t parameter_index = fact->parameter_indices[i];
    IREE_ASSERT(parameter_index < descriptor->parameter_count);

    uint8_t encoded_value_kind = 0;
    IREE_RETURN_IF_ERROR(loom_bytecode_reader_read_u8(
        materializer->decoder, &cursor, &encoded_value_kind));
    IREE_ASSERT(encoded_value_kind < LOOM_BYTECODE_ATTR_COUNT);
    IREE_RETURN_IF_ERROR(loom_bytecode_attribute_materialize_named(
        &attribute_materializer, &cursor,
        &descriptor->parameter_descriptors[parameter_index],
        (loom_bytecode_attr_kind_t)encoded_value_kind,
        &parameters[parameter_index], fact->base.type_id));
  }
  IREE_RETURN_IF_ERROR(loom_bytecode_reader_expect_empty(
      materializer->decoder, &cursor,
      IREE_SV("parameterized_type_parameters")));

  return loom_module_make_parameterized_type(
      materializer->output_module, descriptor, parameters,
      descriptor->parameter_count, out_type, out_type_id);
}

iree_status_t loom_bytecode_type_materialize_structural(
    const loom_bytecode_structural_type_plan_t* plan,
    const loom_bytecode_structural_type_fact_t* fact,
    const loom_type_id_t* dependency_ids, loom_module_t* module,
    loom_type_id_t* out_type_id) {
  if (fact->base.kind == LOOM_TYPE_FUNCTION) {
    loom_func_type_data_t metadata;
    memcpy(&metadata, fact->payload_prefix, sizeof(metadata));
    return loom_module_intern_topological_type_id(
        module, loom_type_function(&metadata), dependency_ids,
        plan->dependency_count, out_type_id);
  }
  if (fact->base.kind == LOOM_TYPE_REGISTER) {
    const loom_register_type_data_t metadata = {
        .carrier_payload0 = fact->payload_prefix[0],
        .carrier_payload1 = fact->payload_prefix[1],
    };
    return loom_module_intern_topological_type_id(
        module, loom_type_register_payload_with_value_type(&metadata),
        dependency_ids, plan->dependency_count, out_type_id);
  }
  return loom_module_intern_topological_type_id(
      module, loom_type_dialect(plan->name_id, plan->parameter_count, NULL),
      dependency_ids, plan->dependency_count, out_type_id);
}

iree_status_t loom_bytecode_type_materialize(
    loom_bytecode_type_materializer_t* materializer) {
  const loom_bytecode_type_fact_t* fact =
      materializer->module_view->types.facts;
  for (iree_host_size_t type_index = 0;
       type_index < materializer->module_view->types.count; ++type_index) {
    IREE_ASSERT(!fact || fact->type_id >= type_index);
    loom_type_t type = {0};
    loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
    iree_status_t status = iree_ok_status();
    const loom_bytecode_type_plan_entry_t* entry =
        &materializer->module_view->types.entries[type_index];
    if (fact && fact->type_id == type_index) {
      if (fact->kind == LOOM_TYPE_PARAMETERIZED) {
        const iree_arena_checkpoint_t checkpoint =
            iree_arena_checkpoint_save(materializer->scratch_arena);
        status = loom_bytecode_type_materialize_parameterized(
            materializer, (const loom_bytecode_parameterized_type_fact_t*)fact,
            &type, &type_id);
        iree_arena_checkpoint_restore(&checkpoint);
      } else {
        const loom_bytecode_structural_type_fact_t* structural_fact =
            (const loom_bytecode_structural_type_fact_t*)fact;
        status = loom_bytecode_type_materialize_structural(
            &entry->structural, structural_fact, structural_fact->type_ids,
            materializer->output_module, &type_id);
      }
      fact = fact->next;
    } else {
      type = entry->direct_type;
    }
    if (iree_status_is_ok(status) && type_id == LOOM_TYPE_ID_INVALID) {
      status = loom_module_intern_topological_type_id(
          materializer->output_module, type, NULL, 0, &type_id);
    }
    if (iree_status_is_ok(status) && type_id != type_index) {
      status = loom_bytecode_reader_emit_invalid_field(
          materializer->decoder, IREE_SV("TYPES"), IREE_SV("type"), type_index,
          IREE_SV("type"),
          materializer->module_view->types.entries[type_index].bytecode_offset,
          IREE_SV("type_table_must_be_deduplicated_and_topologically_ordered"));
    }
    IREE_RETURN_IF_ERROR(status);
  }
  IREE_ASSERT(!fact);
  return iree_ok_status();
}
