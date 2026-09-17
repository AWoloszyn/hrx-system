// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/function_kernel.h"

#include "loom/target/arch/llvmir/descriptors/descriptors.h"

typedef enum loom_llvmir_emit_kernel_query_kind_e {
  LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_ID = 0,
  LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_ID = 1,
  LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_SIZE = 2,
  LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_DISPATCH_ID = 3,
  LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_SIZE = 4,
  LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_COUNT = 5,
} loom_llvmir_emit_kernel_query_kind_t;

typedef enum loom_llvmir_emit_dimension_e {
  LOOM_LLVMIR_EMIT_DIMENSION_X = 0,
  LOOM_LLVMIR_EMIT_DIMENSION_Y = 1,
  LOOM_LLVMIR_EMIT_DIMENSION_Z = 2,
} loom_llvmir_emit_dimension_t;

typedef struct loom_llvmir_emit_kernel_query_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // Logical kernel query represented by the descriptor.
  loom_llvmir_emit_kernel_query_kind_t kind;
  // Query coordinate dimension.
  loom_llvmir_emit_dimension_t dimension;
} loom_llvmir_emit_kernel_query_info_t;

#define LOOM_LLVMIR_KERNEL_QUERY_INFO(query, suffix, kind, dimension)  \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_KERNEL_##query##_##suffix, kind, \
   dimension}

#define LOOM_LLVMIR_KERNEL_SCALAR_QUERY_INFO(query, kind)   \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_KERNEL_##query, kind, \
   LOOM_LLVMIR_EMIT_DIMENSION_X}

#define LOOM_LLVMIR_KERNEL_QUERY_INFOS(query, kind)                            \
  LOOM_LLVMIR_KERNEL_QUERY_INFO(query, X, kind, LOOM_LLVMIR_EMIT_DIMENSION_X), \
      LOOM_LLVMIR_KERNEL_QUERY_INFO(query, Y, kind,                            \
                                    LOOM_LLVMIR_EMIT_DIMENSION_Y),             \
      LOOM_LLVMIR_KERNEL_QUERY_INFO(query, Z, kind,                            \
                                    LOOM_LLVMIR_EMIT_DIMENSION_Z)

static const loom_llvmir_emit_kernel_query_info_t kKernelQueryInfos[] = {
    LOOM_LLVMIR_KERNEL_QUERY_INFOS(WORKITEM_ID,
                                   LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_ID),
    LOOM_LLVMIR_KERNEL_QUERY_INFOS(WORKGROUP_ID,
                                   LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_ID),
    LOOM_LLVMIR_KERNEL_QUERY_INFOS(
        WORKGROUP_SIZE, LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_SIZE),
    LOOM_LLVMIR_KERNEL_QUERY_INFOS(
        WORKITEM_DISPATCH_ID,
        LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_DISPATCH_ID),
    LOOM_LLVMIR_KERNEL_SCALAR_QUERY_INFO(
        SUBGROUP_SIZE, LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_SIZE),
    LOOM_LLVMIR_KERNEL_SCALAR_QUERY_INFO(
        SUBGROUP_COUNT, LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_COUNT),
};

#undef LOOM_LLVMIR_KERNEL_QUERY_INFOS
#undef LOOM_LLVMIR_KERNEL_SCALAR_QUERY_INFO
#undef LOOM_LLVMIR_KERNEL_QUERY_INFO

static const loom_llvmir_emit_kernel_query_info_t*
loom_llvmir_emit_lookup_kernel_query(uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kKernelQueryInfos); ++i) {
    if (kKernelQueryInfos[i].descriptor_ref == descriptor_ref) {
      return &kKernelQueryInfos[i];
    }
  }
  return NULL;
}

static iree_string_view_t loom_llvmir_emit_kernel_coordinate_intrinsic(
    const loom_llvmir_target_profile_t* profile,
    loom_llvmir_emit_kernel_query_kind_t kind,
    loom_llvmir_emit_dimension_t dimension) {
  if (profile->kind != LOOM_LLVMIR_TARGET_PROFILE_KERNEL) {
    return iree_string_view_empty();
  }
  const loom_llvmir_kernel_coordinate_intrinsics_t* intrinsics =
      &profile->kernel.coordinate_intrinsics;
  switch (kind) {
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_ID:
      switch (dimension) {
        case LOOM_LLVMIR_EMIT_DIMENSION_X:
          return intrinsics->workitem_id_x;
        case LOOM_LLVMIR_EMIT_DIMENSION_Y:
          return intrinsics->workitem_id_y;
        case LOOM_LLVMIR_EMIT_DIMENSION_Z:
          return intrinsics->workitem_id_z;
      }
      break;
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_ID:
      switch (dimension) {
        case LOOM_LLVMIR_EMIT_DIMENSION_X:
          return intrinsics->workgroup_id_x;
        case LOOM_LLVMIR_EMIT_DIMENSION_Y:
          return intrinsics->workgroup_id_y;
        case LOOM_LLVMIR_EMIT_DIMENSION_Z:
          return intrinsics->workgroup_id_z;
      }
      break;
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_SIZE:
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_DISPATCH_ID:
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_SIZE:
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_COUNT:
      break;
  }
  return iree_string_view_empty();
}

static uint32_t loom_llvmir_emit_workgroup_size_dimension(
    const loom_llvmir_workgroup_size_t* size,
    loom_llvmir_emit_dimension_t dimension) {
  switch (dimension) {
    case LOOM_LLVMIR_EMIT_DIMENSION_X:
      return size->x;
    case LOOM_LLVMIR_EMIT_DIMENSION_Y:
      return size->y;
    case LOOM_LLVMIR_EMIT_DIMENSION_Z:
      return size->z;
  }
  return 0;
}

static bool loom_llvmir_emit_flat_workgroup_size(
    const loom_llvmir_workgroup_size_t* size, uint32_t* out_flat_size) {
  *out_flat_size = 0;
  if (size->x == 0 || size->y == 0 || size->z == 0) {
    return false;
  }
  const uint64_t flat_size = (uint64_t)size->x * size->y * size->z;
  if (flat_size == 0 || flat_size > UINT32_MAX) {
    return false;
  }
  *out_flat_size = (uint32_t)flat_size;
  return true;
}

static uint32_t loom_llvmir_emit_ceil_div_u32(uint32_t numerator,
                                              uint32_t denominator) {
  IREE_ASSERT_NE(denominator, 0u);
  return numerator == 0 ? 0 : 1u + (numerator - 1u) / denominator;
}

static iree_status_t loom_llvmir_emit_declare_i32_zero_arg_intrinsic(
    loom_llvmir_emit_function_state_t* state, iree_string_view_t name,
    loom_llvmir_function_t** out_function) {
  *out_function = loom_llvmir_module_find_function(state->llvmir_module, name);
  if (*out_function != NULL) {
    return iree_ok_status();
  }

  loom_llvmir_type_id_t i32_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_module_get_integer_type(state->llvmir_module, 32, &i32_type));
  loom_llvmir_function_desc_t desc = {
      .kind = LOOM_LLVMIR_FUNCTION_DECLARATION,
      .name = name,
      .return_type = i32_type,
      .linkage = LOOM_LLVMIR_LINKAGE_DEFAULT,
      .calling_convention = LOOM_LLVMIR_CALLING_CONVENTION_DEFAULT,
      .attr_group_id = LOOM_LLVMIR_ATTR_GROUP_ID_INVALID,
  };
  return loom_llvmir_module_add_function(state->llvmir_module, &desc,
                                         out_function);
}

static iree_status_t loom_llvmir_emit_i32_kernel_coordinate(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_emit_kernel_query_kind_t kind,
    loom_llvmir_emit_dimension_t dimension, loom_llvmir_value_id_t* out_value) {
  iree_string_view_t intrinsic_name =
      loom_llvmir_emit_kernel_coordinate_intrinsic(state->target_profile, kind,
                                                   dimension);
  if (iree_string_view_is_empty(intrinsic_name)) {
    *out_value = LOOM_LLVMIR_VALUE_ID_INVALID;
    return loom_llvmir_emit_unsupported_descriptor_diagnostic(state, packet);
  }
  loom_llvmir_function_t* intrinsic = NULL;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_declare_i32_zero_arg_intrinsic(
      state, intrinsic_name, &intrinsic));
  return loom_llvmir_build_call(
      state->llvmir_block,
      &(loom_llvmir_call_desc_t){
          .callee = loom_llvmir_function_id(intrinsic),
      },
      out_value);
}

static iree_status_t loom_llvmir_emit_i64_kernel_coordinate(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_emit_kernel_query_kind_t kind,
    loom_llvmir_emit_dimension_t dimension, loom_llvmir_type_id_t result_type,
    iree_string_view_t result_name, loom_llvmir_value_id_t* out_value) {
  loom_llvmir_value_id_t i32_coordinate = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_i32_kernel_coordinate(
      state, packet, kind, dimension, &i32_coordinate));
  if (i32_coordinate == LOOM_LLVMIR_VALUE_ID_INVALID) {
    *out_value = LOOM_LLVMIR_VALUE_ID_INVALID;
    return iree_ok_status();
  }
  return loom_llvmir_build_cast(state->llvmir_block,
                                &(loom_llvmir_cast_desc_t){
                                    .result_name = result_name,
                                    .result_type = result_type,
                                    .op = LOOM_LLVMIR_CAST_ZERO_EXTEND,
                                    .value = i32_coordinate,
                                },
                                out_value);
}

static iree_status_t loom_llvmir_emit_workitem_dispatch_id(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_emit_dimension_t dimension, loom_llvmir_type_id_t result_type,
    iree_string_view_t result_name, loom_llvmir_value_id_t* out_value) {
  loom_llvmir_value_id_t workgroup_id = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_i64_kernel_coordinate(
      state, packet, LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_ID, dimension,
      result_type, iree_string_view_empty(), &workgroup_id));
  if (workgroup_id == LOOM_LLVMIR_VALUE_ID_INVALID) {
    *out_value = LOOM_LLVMIR_VALUE_ID_INVALID;
    return iree_ok_status();
  }

  loom_llvmir_value_id_t workitem_id = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_i64_kernel_coordinate(
      state, packet, LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_ID, dimension,
      result_type, iree_string_view_empty(), &workitem_id));
  if (workitem_id == LOOM_LLVMIR_VALUE_ID_INVALID) {
    *out_value = LOOM_LLVMIR_VALUE_ID_INVALID;
    return iree_ok_status();
  }

  const uint32_t workgroup_size = loom_llvmir_emit_workgroup_size_dimension(
      &state->target_profile->kernel.required_workgroup_size, dimension);
  if (workgroup_size == 0) {
    *out_value = LOOM_LLVMIR_VALUE_ID_INVALID;
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("workgroup_size"), workgroup_size, 1);
  }

  loom_llvmir_value_id_t scaled_workgroup_id = workgroup_id;
  if (workgroup_size != 1) {
    loom_llvmir_value_id_t size_constant = LOOM_LLVMIR_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_llvmir_emit_i64_constant(state, workgroup_size, &size_constant));
    IREE_RETURN_IF_ERROR(
        loom_llvmir_build_binop(state->llvmir_block,
                                &(loom_llvmir_binop_desc_t){
                                    .result_type = result_type,
                                    .op = LOOM_LLVMIR_BINOP_MUL,
                                    .lhs = workgroup_id,
                                    .rhs = size_constant,
                                },
                                &scaled_workgroup_id));
  }

  return loom_llvmir_build_binop(state->llvmir_block,
                                 &(loom_llvmir_binop_desc_t){
                                     .result_name = result_name,
                                     .result_type = result_type,
                                     .op = LOOM_LLVMIR_BINOP_ADD,
                                     .lhs = scaled_workgroup_id,
                                     .rhs = workitem_id,
                                 },
                                 out_value);
}

static iree_status_t loom_llvmir_emit_kernel_query(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_kernel_query_info_t* info) {
  if (packet->op->operand_count != 0) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 0);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  switch (info->kind) {
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_ID:
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_ID: {
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_i64_kernel_coordinate(
          state, packet, info->kind, info->dimension, result_type,
          loom_llvmir_emit_value_name(state->module, result_value),
          &llvmir_result));
      break;
    }
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKGROUP_SIZE: {
      const uint32_t workgroup_size = loom_llvmir_emit_workgroup_size_dimension(
          &state->target_profile->kernel.required_workgroup_size,
          info->dimension);
      if (workgroup_size == 0) {
        return loom_llvmir_emit_shape_diagnostic(
            state, packet->op, IREE_SV("workgroup_size"), workgroup_size, 1);
      }
      IREE_RETURN_IF_ERROR(
          loom_llvmir_emit_i64_constant(state, workgroup_size, &llvmir_result));
      break;
    }
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_WORKITEM_DISPATCH_ID: {
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_workitem_dispatch_id(
          state, packet, info->dimension, result_type,
          loom_llvmir_emit_value_name(state->module, result_value),
          &llvmir_result));
      break;
    }
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_SIZE: {
      const uint32_t subgroup_size =
          state->target_profile->kernel.subgroup_size;
      if (subgroup_size == 0) {
        return loom_llvmir_emit_shape_diagnostic(
            state, packet->op, IREE_SV("subgroup_size"), subgroup_size, 1);
      }
      IREE_RETURN_IF_ERROR(
          loom_llvmir_emit_i64_constant(state, subgroup_size, &llvmir_result));
      break;
    }
    case LOOM_LLVMIR_EMIT_KERNEL_QUERY_SUBGROUP_COUNT: {
      const uint32_t subgroup_size =
          state->target_profile->kernel.subgroup_size;
      if (subgroup_size == 0) {
        return loom_llvmir_emit_shape_diagnostic(
            state, packet->op, IREE_SV("subgroup_size"), subgroup_size, 1);
      }
      uint32_t flat_workgroup_size = 0;
      if (!loom_llvmir_emit_flat_workgroup_size(
              &state->target_profile->kernel.required_workgroup_size,
              &flat_workgroup_size)) {
        return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                                 IREE_SV("workgroup_size"),
                                                 flat_workgroup_size, 1);
      }
      const uint32_t subgroup_count =
          loom_llvmir_emit_ceil_div_u32(flat_workgroup_size, subgroup_size);
      IREE_RETURN_IF_ERROR(
          loom_llvmir_emit_i64_constant(state, subgroup_count, &llvmir_result));
      break;
    }
  }
  if (llvmir_result == LOOM_LLVMIR_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_kernel_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet, bool* out_matched) {
  *out_matched = true;
  const uint32_t descriptor_ref = packet->descriptor_ordinal;

  const loom_llvmir_emit_kernel_query_info_t* kernel_query_info =
      loom_llvmir_emit_lookup_kernel_query(descriptor_ref);
  if (kernel_query_info) {
    return loom_llvmir_emit_kernel_query(state, packet, kernel_query_info);
  }

  *out_matched = false;
  return iree_ok_status();
}
