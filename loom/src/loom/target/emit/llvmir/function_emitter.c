// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/function_emitter.h"

#include "loom/codegen/low/diagnostics.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/attribute.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/llvmir/descriptors/descriptors.h"
#include "loom/target/emit/llvmir/function_arithmetic.h"
#include "loom/target/emit/llvmir/function_kernel.h"
#include "loom/target/emit/llvmir/function_memory.h"
#include "loom/target/emit/llvmir/function_vector.h"
#include "loom/target/registers.h"
#include "loom/util/cfg_graph.h"

static iree_string_view_t loom_llvmir_emit_string_or_empty(
    const loom_module_t* module, loom_string_id_t string_id) {
  if (string_id == LOOM_STRING_ID_INVALID ||
      string_id >= module->strings.count) {
    return iree_string_view_empty();
  }
  return module->strings.entries[string_id];
}

iree_string_view_t loom_llvmir_emit_value_name(const loom_module_t* module,
                                               loom_value_id_t value_id) {
  if (value_id >= module->values.count) {
    return iree_string_view_empty();
  }
  const loom_value_t* value = loom_module_value(module, value_id);
  return loom_llvmir_emit_string_or_empty(module, value->name_id);
}

static const loom_named_attr_t* loom_llvmir_emit_find_attr(
    const loom_module_t* module, loom_named_attr_slice_t attrs,
    iree_string_view_t name) {
  const loom_string_id_t name_id = loom_module_lookup_string(module, name);
  if (name_id == LOOM_STRING_ID_INVALID) {
    return NULL;
  }
  for (iree_host_size_t i = 0; i < attrs.count; ++i) {
    const loom_named_attr_t* attr = &attrs.entries[i];
    if (attr->name_id == name_id) {
      return attr;
    }
  }
  return NULL;
}

static loom_named_attr_slice_t loom_llvmir_emit_packet_attrs(
    const loom_low_descriptor_packet_t* packet,
    uint16_t* out_attrs_attr_index) {
  switch (packet->kind) {
    case LOOM_LOW_DESCRIPTOR_PACKET_CONST:
      *out_attrs_attr_index = loom_low_const_attrs_ATTR_INDEX;
      return loom_low_const_attrs(packet->op);
    case LOOM_LOW_DESCRIPTOR_PACKET_OP:
      *out_attrs_attr_index = loom_low_op_attrs_ATTR_INDEX;
      return loom_low_op_attrs(packet->op);
    case LOOM_LOW_DESCRIPTOR_PACKET_NONE:
      break;
  }
  *out_attrs_attr_index = LOOM_ATTR_INDEX_NONE;
  return loom_named_attr_slice_empty();
}

iree_status_t loom_llvmir_emit_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    const loom_error_def_t* error, const loom_diagnostic_param_t* params,
    iree_host_size_t param_count) {
  const loom_diagnostic_emission_t emission = {
      .op = op,
      .error = error,
      .params = params,
      .param_count = param_count,
  };
  IREE_RETURN_IF_ERROR(
      iree_diagnostic_emit(state->diagnostic_emitter, &emission));
  ++state->error_count;
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_shape_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    iree_string_view_t subject_kind, uint32_t actual_count,
    uint32_t expected_count) {
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->function_name),
      loom_param_string(LOOM_LLVMIR_LOW_EMITTER_KEY),
      loom_param_string(subject_kind),
      loom_param_u32(actual_count),
      loom_param_u32(expected_count),
  };
  return loom_llvmir_emit_diagnostic(state, op, LOOM_ERR_TARGET_054, params,
                                     IREE_ARRAYSIZE(params));
}

iree_status_t loom_llvmir_emit_value_type_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    iree_string_view_t expected_constraint) {
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->function_name),
      loom_param_string(value_kind),
      loom_param_string(
          loom_low_diagnostic_value_name(state->module, value_id)),
      loom_param_type(loom_module_value_type(state->module, value_id)),
      loom_param_string(LOOM_LLVMIR_LOW_EMITTER_KEY),
      loom_param_string(expected_constraint),
  };
  return loom_llvmir_emit_diagnostic(state, op, LOOM_ERR_TARGET_056, params,
                                     IREE_ARRAYSIZE(params));
}

iree_status_t loom_llvmir_emit_unsupported_descriptor_diagnostic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  const iree_string_view_t descriptor_key =
      loom_low_descriptor_packet_diagnostic_key(state->target->descriptor_set,
                                                packet);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->function_name),
      loom_param_with_field_ref(
          loom_param_string(descriptor_key),
          loom_diagnostic_field_ref(
              LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE,
              loom_low_descriptor_packet_attribute_index(packet))),
      loom_param_string(state->target->descriptor_set_key),
      loom_param_string(LOOM_LLVMIR_LOW_EMITTER_KEY),
  };
  return loom_llvmir_emit_diagnostic(state, packet->op, LOOM_ERR_TARGET_053,
                                     params, IREE_ARRAYSIZE(params));
}

static iree_status_t loom_llvmir_emit_missing_immediate_diagnostic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    iree_string_view_t immediate_name, uint16_t attrs_attr_index) {
  const iree_string_view_t descriptor_key =
      loom_low_descriptor_packet_diagnostic_key(state->target->descriptor_set,
                                                packet);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->function_name),
      loom_param_with_field_ref(
          loom_param_string(descriptor_key),
          loom_diagnostic_field_ref(
              LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE,
              loom_low_descriptor_packet_attribute_index(packet))),
      loom_param_with_field_ref(
          loom_param_string(immediate_name),
          loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE,
                                    attrs_attr_index)),
  };
  return loom_llvmir_emit_diagnostic(state, packet->op, LOOM_ERR_TARGET_047,
                                     params, IREE_ARRAYSIZE(params));
}

static iree_status_t loom_llvmir_emit_immediate_kind_diagnostic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    iree_string_view_t immediate_name, uint16_t attrs_attr_index,
    loom_attr_kind_t actual_kind) {
  const iree_string_view_t descriptor_key =
      loom_low_descriptor_packet_diagnostic_key(state->target->descriptor_set,
                                                packet);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->function_name),
      loom_param_with_field_ref(
          loom_param_string(descriptor_key),
          loom_diagnostic_field_ref(
              LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE,
              loom_low_descriptor_packet_attribute_index(packet))),
      loom_param_with_field_ref(
          loom_param_string(immediate_name),
          loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE,
                                    attrs_attr_index)),
      loom_param_u32(actual_kind),
      loom_param_string(IREE_SV("i64")),
  };
  return loom_llvmir_emit_diagnostic(state, packet->op, LOOM_ERR_TARGET_049,
                                     params, IREE_ARRAYSIZE(params));
}

static iree_status_t loom_llvmir_emit_register_class_diagnostic(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    loom_diagnostic_field_ref_t field_ref) {
  const loom_type_t type = loom_module_value_type(state->module, value_id);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(state->function_name),
      loom_param_string(value_kind),
      loom_param_string(
          loom_low_diagnostic_value_name(state->module, value_id)),
      loom_param_with_field_ref(loom_param_type(type), field_ref),
      loom_param_string(state->target->descriptor_set_key),
  };
  return loom_llvmir_emit_diagnostic(state, op, LOOM_ERR_TARGET_042, params,
                                     IREE_ARRAYSIZE(params));
}

iree_status_t loom_llvmir_emit_read_i64_immediate(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    iree_string_view_t immediate_name, bool* out_present, int64_t* out_value) {
  *out_present = false;
  uint16_t attrs_attr_index = LOOM_ATTR_INDEX_NONE;
  const loom_named_attr_slice_t attrs =
      loom_llvmir_emit_packet_attrs(packet, &attrs_attr_index);
  const loom_named_attr_t* attr =
      loom_llvmir_emit_find_attr(state->module, attrs, immediate_name);
  if (!attr) {
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_missing_immediate_diagnostic(
        state, packet, immediate_name, attrs_attr_index));
    return iree_ok_status();
  }
  if (attr->value.kind != LOOM_ATTR_I64) {
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_immediate_kind_diagnostic(
        state, packet, immediate_name, attrs_attr_index, attr->value.kind));
    return iree_ok_status();
  }
  *out_value = loom_attr_as_i64(attr->value);
  *out_present = true;
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_read_optional_i64_immediate(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    iree_string_view_t immediate_name, int64_t* inout_value) {
  uint16_t attrs_attr_index = LOOM_ATTR_INDEX_NONE;
  const loom_named_attr_slice_t attrs =
      loom_llvmir_emit_packet_attrs(packet, &attrs_attr_index);
  const loom_named_attr_t* attr =
      loom_llvmir_emit_find_attr(state->module, attrs, immediate_name);
  if (!attr) {
    return iree_ok_status();
  }
  if (attr->value.kind != LOOM_ATTR_I64) {
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_immediate_kind_diagnostic(
        state, packet, immediate_name, attrs_attr_index, attr->value.kind));
    return iree_ok_status();
  }
  *inout_value = loom_attr_as_i64(attr->value);
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_core_scalar_type(
    loom_llvmir_module_t* module, loom_llvmir_emit_core_type_t type,
    uint32_t pointer_address_space, loom_llvmir_type_id_t* out_type_id) {
  switch (type) {
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I1:
      return loom_llvmir_module_get_integer_type(module, 1, out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I8:
      return loom_llvmir_module_get_integer_type(module, 8, out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I16:
      return loom_llvmir_module_get_integer_type(module, 16, out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I32:
      return loom_llvmir_module_get_integer_type(module, 32, out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I64:
      return loom_llvmir_module_get_integer_type(module, 64, out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_F16:
      return loom_llvmir_module_get_float_type(module, LOOM_LLVMIR_FLOAT_F16,
                                               out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_BF16:
      return loom_llvmir_module_get_float_type(module, LOOM_LLVMIR_FLOAT_BF16,
                                               out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_F32:
      return loom_llvmir_module_get_float_type(module, LOOM_LLVMIR_FLOAT_F32,
                                               out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_F64:
      return loom_llvmir_module_get_float_type(module, LOOM_LLVMIR_FLOAT_F64,
                                               out_type_id);
    case LOOM_LLVMIR_EMIT_CORE_TYPE_PTR:
      return loom_llvmir_module_get_pointer_type(module, pointer_address_space,
                                                 out_type_id);
  }
  return iree_make_status(IREE_STATUS_INTERNAL,
                          "unknown LLVMIR low core type %d", (int)type);
}

iree_status_t loom_llvmir_emit_core_type(loom_llvmir_module_t* module,
                                         loom_llvmir_emit_core_type_t type,
                                         uint32_t unit_count,
                                         uint32_t pointer_address_space,
                                         loom_llvmir_type_id_t* out_type_id) {
  loom_llvmir_type_id_t scalar_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_core_scalar_type(
      module, type, pointer_address_space, &scalar_type));
  if (unit_count == 1) {
    *out_type_id = scalar_type;
    return iree_ok_status();
  }
  return loom_llvmir_module_get_vector_type(module, unit_count, scalar_type,
                                            out_type_id);
}

bool loom_llvmir_emit_low_value_is_pointer_register(
    loom_llvmir_emit_function_state_t* state, loom_value_id_t value_id) {
  if (value_id >= state->module->values.count) {
    return false;
  }
  const loom_type_t type = loom_module_value_type(state->module, value_id);
  const loom_low_register_type_resolver_t resolver =
      loom_low_register_type_resolver_for_descriptor_set(
          state->target->descriptor_set);
  uint16_t reg_class_id = LOOM_LOW_REG_CLASS_NONE;
  if (!loom_low_register_type_resolver_try_resolve(&resolver, type,
                                                   &reg_class_id, NULL)) {
    return false;
  }
  return reg_class_id == LLVMIR_GENERIC_CORE_REG_CLASS_ID_PTR &&
         loom_low_register_type_unit_count(type) == 1;
}

iree_status_t loom_llvmir_emit_type_for_low_value(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    loom_diagnostic_field_ref_t field_ref, loom_llvmir_type_id_t* out_type_id) {
  *out_type_id = LOOM_LLVMIR_TYPE_ID_INVALID;
  const loom_type_t type = loom_module_value_type(state->module, value_id);
  const loom_low_register_type_resolver_t resolver =
      loom_low_register_type_resolver_for_descriptor_set(
          state->target->descriptor_set);
  uint16_t reg_class_id = LOOM_LOW_REG_CLASS_NONE;
  if (!loom_low_register_type_resolver_try_resolve(&resolver, type,
                                                   &reg_class_id, NULL)) {
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_register_class_diagnostic(
        state, op, value_id, value_kind, field_ref));
    return iree_ok_status();
  }
  loom_llvmir_emit_core_type_t core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_I1;
  switch (reg_class_id) {
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_I1:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_I1;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_I8:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_I8;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_I16:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_I16;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_I32:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_I32;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_I64:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_I64;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_F16:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_F16;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_BF16:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_BF16;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_F32:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_F32;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_F64:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_F64;
      break;
    case LLVMIR_GENERIC_CORE_REG_CLASS_ID_PTR:
      core_type = LOOM_LLVMIR_EMIT_CORE_TYPE_PTR;
      break;
    default: {
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_register_class_diagnostic(
          state, op, value_id, value_kind, field_ref));
      return iree_ok_status();
    }
  }
  uint32_t pointer_address_space = 0;
  if (core_type == LOOM_LLVMIR_EMIT_CORE_TYPE_PTR) {
    bool supported = true;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_pointer_address_space_for_low_value(
        state, op, value_id, value_kind, IREE_SV("reg<llvmir.ptr>"),
        &pointer_address_space, &supported));
    if (!supported) {
      return iree_ok_status();
    }
  }
  return loom_llvmir_emit_core_type(state->llvmir_module, core_type,
                                    loom_low_register_type_unit_count(type),
                                    pointer_address_space, out_type_id);
}

iree_status_t loom_llvmir_emit_prepare_packet_result(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_type_id_t* out_result_type, loom_value_id_t* out_result_value) {
  if (packet->op->result_count != 1) {
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_result"), packet->op->result_count,
        1));
    *out_result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
    *out_result_value = LOOM_VALUE_ID_INVALID;
    return iree_ok_status();
  }
  const loom_value_id_t result_value = loom_op_const_results(packet->op)[0];
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_type_for_low_value(
      state, packet->op, result_value, IREE_SV("result"),
      loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_RESULT, 0),
      out_result_type));
  *out_result_value = result_value;
  return iree_ok_status();
}

uint32_t loom_llvmir_emit_low_value_unit_count(
    loom_llvmir_emit_function_state_t* state, loom_value_id_t value_id) {
  return loom_low_register_type_unit_count(
      loom_module_value_type(state->module, value_id));
}

iree_status_t loom_llvmir_emit_i64_constant(
    loom_llvmir_emit_function_state_t* state, int64_t value,
    loom_llvmir_value_id_t* out_value) {
  loom_llvmir_type_id_t i64_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_module_get_integer_type(state->llvmir_module, 64, &i64_type));
  return loom_llvmir_module_add_integer_constant(state->llvmir_module, i64_type,
                                                 (uint64_t)value, out_value);
}

static iree_status_t loom_llvmir_emit_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  if (packet->kind == LOOM_LOW_DESCRIPTOR_PACKET_CONST) {
    return loom_llvmir_emit_constant_packet(state, packet);
  }
  bool matched = false;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_emit_arithmetic_packet(state, packet, &matched));
  if (matched) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_kernel_packet(state, packet, &matched));
  if (matched) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_vector_packet(state, packet, &matched));
  if (matched) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_memory_packet(state, packet, &matched));
  if (matched) {
    return iree_ok_status();
  }
  return loom_llvmir_emit_unsupported_descriptor_diagnostic(state, packet);
}

static void loom_llvmir_emit_transfer(loom_llvmir_emit_function_state_t* state,
                                      const loom_op_t* op) {
  const loom_llvmir_value_id_t source =
      loom_llvmir_emit_lookup_value(state, loom_op_const_operands(op)[0]);
  loom_llvmir_emit_define_value(state, loom_op_const_results(op)[0], source);
}

static iree_status_t loom_llvmir_emit_return(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op) {
  const loom_value_slice_t values = loom_low_return_values(op);
  if (values.count != state->result_count) {
    return loom_llvmir_emit_shape_diagnostic(state, op,
                                             IREE_SV("return_operand"),
                                             values.count, state->result_count);
  }
  if (values.count == 0) {
    return loom_llvmir_build_ret_void(state->llvmir_block);
  }
  const loom_llvmir_value_id_t value =
      loom_llvmir_emit_lookup_value(state, values.values[0]);
  return loom_llvmir_build_ret(state->llvmir_block, value);
}

static void loom_llvmir_emit_compile_time_op(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op) {
  if (op->result_count == 0) {
    return;
  }
  IREE_ASSERT(loom_traits_are_fact_identity(op->traits));
  IREE_ASSERT_EQ(op->operand_count, op->result_count);
  const loom_value_id_t* operands = loom_op_const_operands(op);
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    const loom_llvmir_value_id_t value =
        loom_llvmir_emit_lookup_value(state, operands[i]);
    loom_llvmir_emit_define_value(state, results[i], value);
  }
}

static iree_status_t loom_llvmir_emit_low_op(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op) {
  if (loom_traits_are_compile_time_only(op->traits)) {
    loom_llvmir_emit_compile_time_op(state, op);
    return iree_ok_status();
  }
  if (loom_low_return_isa(op)) {
    return loom_llvmir_emit_return(state, op);
  }
  if (loom_low_br_isa(op)) {
    return loom_llvmir_build_br(
        state->llvmir_block,
        loom_llvmir_block_id(
            state->block_map[loom_low_br_dest(op)->region_index]));
  }
  if (loom_low_cond_br_isa(op)) {
    const loom_llvmir_value_id_t condition =
        loom_llvmir_emit_lookup_value(state, loom_low_cond_br_condition(op));
    return loom_llvmir_build_cond_br(
        state->llvmir_block, condition,
        loom_llvmir_block_id(
            state->block_map[loom_low_cond_br_true_dest(op)->region_index]),
        loom_llvmir_block_id(
            state->block_map[loom_low_cond_br_false_dest(op)->region_index]));
  }
  if (loom_low_resource_isa(op)) {
    return iree_ok_status();
  }
  if (loom_low_copy_isa(op) || loom_low_move_isa(op)) {
    loom_llvmir_emit_transfer(state, op);
    return iree_ok_status();
  }

  loom_low_descriptor_packet_t packet = {0};
  loom_low_descriptor_packet_initialize(state->target->descriptor_set, op,
                                        &packet);
  if (packet.kind != LOOM_LOW_DESCRIPTOR_PACKET_NONE) {
    return loom_llvmir_emit_packet(state, &packet);
  }
  return loom_llvmir_emit_shape_diagnostic(state, op, IREE_SV("packet_kind"), 0,
                                           1);
}

static iree_status_t loom_llvmir_emit_initialize_value_map(
    loom_llvmir_emit_function_state_t* state) {
  IREE_RETURN_IF_ERROR(loom_local_value_domain_acquire_for_region(
      state->module, state->body, state->scratch_arena, &state->value_domain));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      state->scratch_arena, state->value_domain.value_count,
      sizeof(*state->value_map), (void**)&state->value_map));
  for (iree_host_size_t i = 0; i < state->value_domain.value_count; ++i) {
    state->value_map[i] = LOOM_LLVMIR_VALUE_ID_INVALID;
  }
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_resource_parameter(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* resource_op) {
  const loom_value_id_t result_value = loom_low_resource_result(resource_op);
  uint32_t pointer_address_space = 0;
  bool supported = true;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_pointer_address_space_for_low_value(
      state, resource_op, result_value, IREE_SV("resource"),
      IREE_SV("native_pointer or hal_binding pointer resource"),
      &pointer_address_space, &supported));
  if (!supported) {
    return iree_ok_status();
  }

  loom_llvmir_type_id_t parameter_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_get_pointer_type(
      state->llvmir_module, pointer_address_space, &parameter_type));

  loom_llvmir_attr_t
      binding_attrs[LOOM_LLVMIR_TARGET_PROFILE_MAX_KERNEL_BINDING_ATTR_COUNT] =
          {{0}};
  iree_host_size_t binding_attr_count = 0;
  if (state->target_profile->kind == LOOM_LLVMIR_TARGET_PROFILE_KERNEL &&
      loom_low_resource_import_kind(resource_op) ==
          LOOM_LOW_RESOURCE_IMPORT_KIND_HAL_BINDING) {
    loom_llvmir_target_profile_kernel_binding_attrs(
        state->target_profile, binding_attrs, &binding_attr_count);
  }

  loom_llvmir_value_id_t parameter = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_function_add_parameter(
      state->llvmir_function,
      &(loom_llvmir_parameter_desc_t){
          .type_id = parameter_type,
          .name = loom_llvmir_emit_value_name(state->module, result_value),
          .attrs = binding_attrs,
          .attr_count = binding_attr_count,
      },
      &parameter));
  loom_llvmir_emit_define_value(state, result_value, parameter);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_resource_parameters(
    loom_llvmir_emit_function_state_t* state) {
  const loom_block_t* entry_block = loom_region_const_entry_block(state->body);
  loom_op_t* op = NULL;
  loom_block_for_each_op(entry_block, op) {
    if (!loom_low_resource_isa(op)) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_resource_parameter(state, op));
  }
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_function_signature(
    loom_llvmir_emit_function_state_t* state) {
  const loom_value_slice_t results =
      loom_low_func_def_results(state->function_op);
  state->result_count = results.count;
  if (results.count > 1) {
    return loom_llvmir_emit_shape_diagnostic(state, state->function_op,
                                             IREE_SV("function_result"),
                                             results.count, 1);
  }

  loom_llvmir_type_id_t return_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  if (results.count == 0) {
    IREE_RETURN_IF_ERROR(
        loom_llvmir_module_get_void_type(state->llvmir_module, &return_type));
  } else {
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_type_for_low_value(
        state, state->function_op, results.values[0], IREE_SV("result"),
        loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_RESULT, 0),
        &return_type));
  }
  if (return_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const loom_block_t* entry_block = loom_region_const_entry_block(state->body);
  loom_llvmir_type_id_t* arg_types = NULL;
  if (entry_block->arg_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(state->scratch_arena, entry_block->arg_count,
                                  sizeof(*arg_types), (void**)&arg_types));
  }
  bool valid_signature = true;
  for (uint16_t i = 0; i < entry_block->arg_count; ++i) {
    const loom_value_id_t arg_value = entry_block->arg_ids[i];
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_type_for_low_value(
        state, state->function_op, arg_value, IREE_SV("parameter"),
        loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_OPERAND, i),
        &arg_types[i]));
    if (arg_types[i] == LOOM_LLVMIR_TYPE_ID_INVALID) {
      valid_signature = false;
    }
  }
  if (!valid_signature) {
    return iree_ok_status();
  }

  const iree_string_view_t export_symbol =
      loom_low_resolved_target_bundle(state->target)
          ->export_plan->export_symbol;
  const iree_string_view_t function_name =
      iree_string_view_is_empty(export_symbol) ? state->function_name
                                               : export_symbol;
  loom_llvmir_attr_group_id_t attr_group_id = LOOM_LLVMIR_ATTR_GROUP_ID_INVALID;
  loom_llvmir_calling_convention_t calling_convention =
      LOOM_LLVMIR_CALLING_CONVENTION_DEFAULT;
  if (state->target_profile->kind == LOOM_LLVMIR_TARGET_PROFILE_KERNEL) {
    IREE_RETURN_IF_ERROR(loom_llvmir_target_profile_add_kernel_attr_group(
        state->llvmir_module, state->target_profile, &attr_group_id));
    calling_convention = state->target_profile->kernel.calling_convention;
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_function(
      state->llvmir_module,
      &(loom_llvmir_function_desc_t){
          .kind = LOOM_LLVMIR_FUNCTION_DEFINITION,
          .name = function_name,
          .return_type = return_type,
          .linkage = state->target_profile->exported_linkage,
          .calling_convention = calling_convention,
          .attr_group_id = attr_group_id,
      },
      &state->llvmir_function));

  for (uint16_t i = 0; i < entry_block->arg_count; ++i) {
    const loom_value_id_t arg_value = entry_block->arg_ids[i];
    loom_llvmir_value_id_t parameter = LOOM_LLVMIR_VALUE_ID_INVALID;
    loom_llvmir_attr_t binding_attrs
        [LOOM_LLVMIR_TARGET_PROFILE_MAX_KERNEL_BINDING_ATTR_COUNT] = {{0}};
    iree_host_size_t binding_attr_count = 0;
    if (state->target_profile->kind == LOOM_LLVMIR_TARGET_PROFILE_KERNEL &&
        loom_llvmir_emit_low_value_is_pointer_register(state, arg_value)) {
      loom_llvmir_target_profile_kernel_binding_attrs(
          state->target_profile, binding_attrs, &binding_attr_count);
    }
    IREE_RETURN_IF_ERROR(loom_llvmir_function_add_parameter(
        state->llvmir_function,
        &(loom_llvmir_parameter_desc_t){
            .type_id = arg_types[i],
            .name = loom_llvmir_emit_value_name(state->module, arg_value),
            .attrs = binding_attrs,
            .attr_count = binding_attr_count,
        },
        &parameter));
    loom_llvmir_emit_define_value(state, arg_value, parameter);
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_resource_parameters(state));
  if (state->target_profile->kind == LOOM_LLVMIR_TARGET_PROFILE_KERNEL) {
    IREE_RETURN_IF_ERROR(loom_llvmir_target_profile_attach_kernel_metadata(
        state->llvmir_function, state->target_profile));
  }

  return iree_ok_status();
}

// LLVM requires a predecessor-free entry block. A Low entry with incoming
// edges receives initial ABI parameters through a separate LLVM block.
typedef struct loom_llvmir_emit_entry_t {
  // ABI preheader for a Low entry with incoming edges; otherwise NULL.
  loom_llvmir_block_t* block;
  // Initial parameter identities, indexed by Low entry argument ordinal.
  loom_llvmir_value_id_t* parameters;
} loom_llvmir_emit_entry_t;

static iree_status_t loom_llvmir_emit_declare_blocks(
    loom_llvmir_emit_function_state_t* state, const loom_cfg_graph_t* graph,
    const loom_llvmir_emit_entry_t* entry) {
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      state->scratch_arena, graph->block_count, sizeof(*state->block_map),
      (void**)&state->block_map));
  // Serialize blocks in the same dominance-respecting order used for bodies.
  // Bitcode operands then reference earlier definitions, except phi backedges.
  for (iree_host_size_t ordinal = 0; ordinal < graph->reverse_postorder.count;
       ++ordinal) {
    const uint16_t i = graph->reverse_postorder.values[ordinal];
    const loom_block_t* block = graph->blocks[i].block;
    const iree_string_view_t name =
        i == 0
            ? IREE_SV("entry")
            : loom_llvmir_emit_string_or_empty(state->module, block->label_id);
    IREE_RETURN_IF_ERROR(loom_llvmir_function_add_block(
        state->llvmir_function, name, &state->block_map[i]));
    if (i == 0 && !entry->block) {
      continue;
    }
    for (uint16_t j = 0; j < block->arg_count; ++j) {
      const loom_value_id_t argument = loom_block_arg_id(block, j);
      // A parameter's ABI address space does not describe pointers carried
      // back to the entry. The Low ptr register type retains no such fact.
      if (i == 0 &&
          loom_llvmir_emit_low_value_is_pointer_register(state, argument)) {
        return loom_llvmir_emit_value_type_diagnostic(
            state, block->first_op, argument, IREE_SV("block_argument"),
            IREE_SV("non-pointer block argument"));
      }
      loom_llvmir_type_id_t type;
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_type_for_low_value(
          state, block->first_op, argument, IREE_SV("block_argument"),
          loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_OPERAND, j), &type));
      if (state->error_count != 0) {
        return iree_ok_status();
      }
      loom_llvmir_value_id_t phi;
      IREE_RETURN_IF_ERROR(loom_llvmir_build_phi(
          state->block_map[i],
          &(loom_llvmir_phi_desc_t){
              .result_name =
                  loom_llvmir_emit_value_name(state->module, argument),
              .result_type = type,
          },
          &phi));
      loom_llvmir_emit_define_value(state, argument, phi);
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_phi_incoming(
    loom_llvmir_emit_function_state_t* state, const loom_cfg_graph_t* graph,
    const loom_llvmir_emit_entry_t* entry) {
  for (uint16_t i = entry->block ? 0 : 1; i < graph->block_count; ++i) {
    const loom_block_t* block = graph->blocks[i].block;
    if (!graph->blocks[i].reachable || block->arg_count == 0) {
      continue;
    }
    const loom_cfg_edge_index_span_t edges =
        loom_cfg_graph_predecessor_edges(graph, i);
    loom_llvmir_phi_incoming_t* incoming = NULL;
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(state->scratch_arena, edges.count + (i == 0),
                                  sizeof(*incoming), (void**)&incoming));
    for (uint16_t j = 0; j < block->arg_count; ++j) {
      iree_host_size_t incoming_count = 0;
      if (i == 0) {
        incoming[incoming_count++] = (loom_llvmir_phi_incoming_t){
            .value = entry->parameters[j],
            .predecessor = loom_llvmir_block_id(entry->block),
        };
      }
      for (iree_host_size_t e = 0; e < edges.count; ++e) {
        const loom_cfg_edge_info_t* edge = &graph->edges[edges.values[e]];
        if (!graph->blocks[edge->source_block_index].reachable) {
          continue;
        }
        const loom_value_slice_t arguments = loom_low_br_args(edge->terminator);
        incoming[incoming_count].value =
            loom_llvmir_emit_lookup_value(state, arguments.values[j]);
        incoming[incoming_count++].predecessor =
            loom_llvmir_block_id(state->block_map[edge->source_block_index]);
      }
      const loom_llvmir_value_id_t phi =
          loom_llvmir_emit_lookup_value(state, loom_block_arg_id(block, j));
      IREE_RETURN_IF_ERROR(loom_llvmir_set_phi_incoming(
          state->block_map[i], phi, incoming, incoming_count));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_function_body(
    loom_llvmir_emit_function_state_t* state) {
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_function_signature(state));
  if (state->llvmir_function == NULL) {
    return iree_ok_status();
  }
  loom_cfg_graph_t graph;
  IREE_RETURN_IF_ERROR(loom_cfg_graph_build(state->module, state->body,
                                            state->scratch_arena, &graph));
  loom_llvmir_emit_entry_t entry = {0};
  if (loom_cfg_graph_predecessor_edges(&graph, 0).count != 0) {
    IREE_RETURN_IF_ERROR(loom_llvmir_function_add_block(
        state->llvmir_function, IREE_SV("abi"), &entry.block));
    const loom_block_t* block = graph.blocks[0].block;
    if (block->arg_count != 0) {
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          state->scratch_arena, block->arg_count, sizeof(*entry.parameters),
          (void**)&entry.parameters));
      for (uint16_t i = 0; i < block->arg_count; ++i) {
        entry.parameters[i] =
            loom_llvmir_emit_lookup_value(state, loom_block_arg_id(block, i));
      }
    }
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_declare_blocks(state, &graph, &entry));
  if (state->error_count != 0) {
    return iree_ok_status();
  }
  if (entry.block) {
    IREE_RETURN_IF_ERROR(loom_llvmir_build_br(
        entry.block, loom_llvmir_block_id(state->block_map[0])));
  }
  for (iree_host_size_t i = 0; i < graph.reverse_postorder.count; ++i) {
    const uint16_t index = graph.reverse_postorder.values[i];
    state->llvmir_block = state->block_map[index];
    loom_op_t* op = NULL;
    loom_block_for_each_op(graph.blocks[index].block, op) {
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_low_op(state, op));
      if (state->error_count != 0) {
        return iree_ok_status();
      }
    }
  }
  return loom_llvmir_emit_phi_incoming(state, &graph, &entry);
}

iree_status_t loom_llvmir_emit_function(
    loom_llvmir_emit_function_state_t* state) {
  iree_status_t status = loom_llvmir_emit_initialize_value_map(state);
  if (iree_status_is_ok(status)) {
    status = loom_llvmir_emit_function_body(state);
  }
  loom_local_value_domain_release(&state->value_domain);
  return status;
}
