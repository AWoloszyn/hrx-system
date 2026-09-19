// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/function_vector.h"

#include <inttypes.h>
#include <stdio.h>

#include "loom/target/arch/llvmir/descriptors/descriptors.h"

#define LOOM_LLVMIR_MAX_VECTOR_LANES 32

typedef enum loom_llvmir_emit_const_kind_e {
  LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER = 0,
  LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS = 1,
} loom_llvmir_emit_const_kind_t;

typedef enum loom_llvmir_emit_shuffle_kind_e {
  LOOM_LLVMIR_EMIT_SHUFFLE_KIND_EXPLICIT = 0,
  LOOM_LLVMIR_EMIT_SHUFFLE_KIND_SLICE = 1,
} loom_llvmir_emit_shuffle_kind_t;

typedef enum loom_llvmir_emit_extract_kind_e {
  LOOM_LLVMIR_EMIT_EXTRACT_KIND_STATIC = 0,
  LOOM_LLVMIR_EMIT_EXTRACT_KIND_DYNAMIC = 1,
} loom_llvmir_emit_extract_kind_t;

typedef struct loom_llvmir_emit_const_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // Scalar value type stored by the constant.
  loom_llvmir_emit_core_type_t value_type;
  // Number of scalar units in the constant result.
  uint32_t unit_count;
  // Immediate payload interpretation for the constant.
  loom_llvmir_emit_const_kind_t kind;
} loom_llvmir_emit_const_info_t;

typedef struct loom_llvmir_emit_concat_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // Number of vector operands concatenated into the result.
  uint32_t input_count;
} loom_llvmir_emit_concat_info_t;

#define LOOM_LLVMIR_CONST_INFO(suffix, type, units, const_kind) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_CONST_##suffix, type, units, const_kind}

#define LOOM_LLVMIR_VECTOR_CONST_INFOS(lanes)                                 \
  LOOM_LLVMIR_CONST_INFO(V##lanes##I8, LOOM_LLVMIR_EMIT_CORE_TYPE_I8, lanes,  \
                         LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),                \
      LOOM_LLVMIR_CONST_INFO(V##lanes##I16, LOOM_LLVMIR_EMIT_CORE_TYPE_I16,   \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),     \
      LOOM_LLVMIR_CONST_INFO(V##lanes##I32, LOOM_LLVMIR_EMIT_CORE_TYPE_I32,   \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),     \
      LOOM_LLVMIR_CONST_INFO(V##lanes##I64, LOOM_LLVMIR_EMIT_CORE_TYPE_I64,   \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),     \
      LOOM_LLVMIR_CONST_INFO(V##lanes##F16, LOOM_LLVMIR_EMIT_CORE_TYPE_F16,   \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),  \
      LOOM_LLVMIR_CONST_INFO(V##lanes##BF16, LOOM_LLVMIR_EMIT_CORE_TYPE_BF16, \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),  \
      LOOM_LLVMIR_CONST_INFO(V##lanes##F32, LOOM_LLVMIR_EMIT_CORE_TYPE_F32,   \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),  \
      LOOM_LLVMIR_CONST_INFO(V##lanes##F64, LOOM_LLVMIR_EMIT_CORE_TYPE_F64,   \
                             lanes, LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS)

static const loom_llvmir_emit_const_info_t kConstInfos[] = {
    LOOM_LLVMIR_CONST_INFO(I1, LOOM_LLVMIR_EMIT_CORE_TYPE_I1, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),
    LOOM_LLVMIR_CONST_INFO(I8, LOOM_LLVMIR_EMIT_CORE_TYPE_I8, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),
    LOOM_LLVMIR_CONST_INFO(I16, LOOM_LLVMIR_EMIT_CORE_TYPE_I16, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),
    LOOM_LLVMIR_CONST_INFO(I32, LOOM_LLVMIR_EMIT_CORE_TYPE_I32, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),
    LOOM_LLVMIR_CONST_INFO(I64, LOOM_LLVMIR_EMIT_CORE_TYPE_I64, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER),
    LOOM_LLVMIR_CONST_INFO(F16, LOOM_LLVMIR_EMIT_CORE_TYPE_F16, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),
    LOOM_LLVMIR_CONST_INFO(BF16, LOOM_LLVMIR_EMIT_CORE_TYPE_BF16, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),
    LOOM_LLVMIR_CONST_INFO(F32, LOOM_LLVMIR_EMIT_CORE_TYPE_F32, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),
    LOOM_LLVMIR_CONST_INFO(F64, LOOM_LLVMIR_EMIT_CORE_TYPE_F64, 1,
                           LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS),
    LOOM_LLVMIR_VECTOR_CONST_INFOS(2),
    LOOM_LLVMIR_VECTOR_CONST_INFOS(3),
    LOOM_LLVMIR_VECTOR_CONST_INFOS(4),
    LOOM_LLVMIR_VECTOR_CONST_INFOS(8),
    LOOM_LLVMIR_VECTOR_CONST_INFOS(16),
};

#undef LOOM_LLVMIR_VECTOR_CONST_INFOS
#undef LOOM_LLVMIR_CONST_INFO

#define LOOM_LLVMIR_VECTOR_REF(prefix, suffix) \
  LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##prefix##_##suffix

#define LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, lanes) \
  LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##I1),           \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##I8),       \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##I16),      \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##I32),      \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##I64),      \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##F16),      \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##BF16),     \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##F32),      \
      LOOM_LLVMIR_VECTOR_REF(prefix, V##lanes##F64)

#define LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(prefix) \
  LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, 2),       \
      LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, 3),   \
      LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, 4),   \
      LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, 8),   \
      LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, 16),  \
      LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS(prefix, 32)

#define LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(lanes)         \
  LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##I1),       \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##I8),   \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##I16),  \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##I32),  \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##I64),  \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##F16),  \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##BF16), \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##F32),  \
      LOOM_LLVMIR_VECTOR_REF(INSERT_DYNAMIC, V##lanes##F64)

#define LOOM_LLVMIR_ALL_DYNAMIC_INSERT_VECTOR_REFS() \
  LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(2),         \
      LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(3),     \
      LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(4),     \
      LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(8),     \
      LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(16),    \
      LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS(32)

static const uint32_t kSplatDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(SPLAT),
};

static const uint32_t kFromElementsDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(FROM_ELEMENTS),
};

static const uint32_t kExtractDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(EXTRACT),
    LOOM_LLVMIR_VECTOR_REF(EXTRACT, V64I8),
};

static const uint32_t kDynamicExtractDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(EXTRACT_DYNAMIC),
    LOOM_LLVMIR_VECTOR_REF(EXTRACT_DYNAMIC, V64I8),
};

static const uint32_t kInsertDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(INSERT),
};

static const uint32_t kDynamicInsertDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_DYNAMIC_INSERT_VECTOR_REFS(),
};

static const uint32_t kShuffleDescriptorRefs[] = {
    LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS(SHUFFLE),
};

#define LOOM_LLVMIR_CONCAT_REF(suffix) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_CONCAT_V4##suffix##_V16##suffix, 4}

static const loom_llvmir_emit_concat_info_t kConcatInfos[] = {
    LOOM_LLVMIR_CONCAT_REF(I1),   LOOM_LLVMIR_CONCAT_REF(I8),
    LOOM_LLVMIR_CONCAT_REF(I16),  LOOM_LLVMIR_CONCAT_REF(I32),
    LOOM_LLVMIR_CONCAT_REF(I64),  LOOM_LLVMIR_CONCAT_REF(F16),
    LOOM_LLVMIR_CONCAT_REF(BF16), LOOM_LLVMIR_CONCAT_REF(F32),
    LOOM_LLVMIR_CONCAT_REF(F64),
};

#define LOOM_LLVMIR_SLICE_REF(source_lanes, result_lanes, suffix) \
  LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_SLICE_V##source_lanes##suffix##_V##result_lanes##suffix

static const uint32_t kSliceDescriptorRefs[] = {
    LOOM_LLVMIR_SLICE_REF(4, 2, I1),   LOOM_LLVMIR_SLICE_REF(4, 2, I8),
    LOOM_LLVMIR_SLICE_REF(4, 2, I16),  LOOM_LLVMIR_SLICE_REF(4, 2, I32),
    LOOM_LLVMIR_SLICE_REF(4, 2, I64),  LOOM_LLVMIR_SLICE_REF(4, 2, F16),
    LOOM_LLVMIR_SLICE_REF(4, 2, BF16), LOOM_LLVMIR_SLICE_REF(4, 2, F32),
    LOOM_LLVMIR_SLICE_REF(4, 2, F64),  LOOM_LLVMIR_SLICE_REF(16, 4, I1),
    LOOM_LLVMIR_SLICE_REF(16, 4, I8),  LOOM_LLVMIR_SLICE_REF(16, 4, I16),
    LOOM_LLVMIR_SLICE_REF(16, 4, I32), LOOM_LLVMIR_SLICE_REF(16, 4, I64),
    LOOM_LLVMIR_SLICE_REF(16, 4, F16), LOOM_LLVMIR_SLICE_REF(16, 4, BF16),
    LOOM_LLVMIR_SLICE_REF(16, 4, F32), LOOM_LLVMIR_SLICE_REF(16, 4, F64),
};

#undef LOOM_LLVMIR_SLICE_REF
#undef LOOM_LLVMIR_CONCAT_REF
#undef LOOM_LLVMIR_ALL_DYNAMIC_INSERT_VECTOR_REFS
#undef LOOM_LLVMIR_DYNAMIC_INSERT_VECTOR_REFS
#undef LOOM_LLVMIR_ALL_STRUCTURAL_VECTOR_REFS
#undef LOOM_LLVMIR_STRUCTURAL_VECTOR_REFS
#undef LOOM_LLVMIR_VECTOR_REF

static const loom_llvmir_emit_const_info_t* loom_llvmir_emit_lookup_const(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kConstInfos); ++i) {
    if (kConstInfos[i].descriptor_ref == descriptor_ref) {
      return &kConstInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_concat_info_t* loom_llvmir_emit_lookup_concat(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kConcatInfos); ++i) {
    if (kConcatInfos[i].descriptor_ref == descriptor_ref) {
      return &kConcatInfos[i];
    }
  }
  return NULL;
}

static iree_status_t loom_llvmir_emit_insert_vector_lane(
    loom_llvmir_emit_function_state_t* state, loom_llvmir_type_id_t result_type,
    loom_llvmir_value_id_t vector, loom_llvmir_value_id_t element,
    uint32_t lane, iree_string_view_t result_name,
    loom_llvmir_value_id_t* out_value) {
  loom_llvmir_value_id_t lane_index = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_i64_constant(state, lane, &lane_index));
  return loom_llvmir_build_insert_element(state->llvmir_block,
                                          &(loom_llvmir_insert_element_desc_t){
                                              .result_name = result_name,
                                              .result_type = result_type,
                                              .vector = vector,
                                              .element = element,
                                              .index = lane_index,
                                          },
                                          out_value);
}

static iree_status_t loom_llvmir_emit_extract_vector_lane(
    loom_llvmir_emit_function_state_t* state, loom_llvmir_type_id_t result_type,
    loom_llvmir_value_id_t vector, uint32_t lane,
    iree_string_view_t result_name, loom_llvmir_value_id_t* out_value) {
  loom_llvmir_value_id_t lane_index = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_i64_constant(state, lane, &lane_index));
  return loom_llvmir_build_extract_element(
      state->llvmir_block,
      &(loom_llvmir_extract_element_desc_t){
          .result_name = result_name,
          .result_type = result_type,
          .vector = vector,
          .index = lane_index,
      },
      out_value);
}

static iree_status_t loom_llvmir_emit_const(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_const_info_t* info) {
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const uint32_t result_unit_count =
      loom_llvmir_emit_low_value_unit_count(state, result_value);
  if (result_unit_count != info->unit_count) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("vector_lane_count"), result_unit_count,
        info->unit_count);
  }
  if (info->unit_count > LOOM_LLVMIR_MAX_VECTOR_LANES) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("vector_lane_count"), info->unit_count,
        LOOM_LLVMIR_MAX_VECTOR_LANES);
  }

  int64_t immediate = 0;
  bool has_immediate = false;
  switch (info->kind) {
    case LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER: {
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
          state, packet, IREE_SV("value"), &has_immediate, &immediate));
      break;
    }
    case LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS: {
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
          state, packet, IREE_SV("bits"), &has_immediate, &immediate));
      break;
    }
  }
  if (!has_immediate) {
    return iree_ok_status();
  }

  loom_llvmir_value_id_t constant = LOOM_LLVMIR_VALUE_ID_INVALID;
  switch (info->kind) {
    case LOOM_LLVMIR_EMIT_CONST_KIND_INTEGER: {
      if (info->unit_count == 1) {
        IREE_RETURN_IF_ERROR(loom_llvmir_module_add_integer_constant(
            state->llvmir_module, result_type, (uint64_t)immediate, &constant));
      } else {
        uint64_t values[LOOM_LLVMIR_MAX_VECTOR_LANES] = {0};
        for (uint32_t i = 0; i < info->unit_count; ++i) {
          values[i] = (uint64_t)immediate;
        }
        IREE_RETURN_IF_ERROR(loom_llvmir_module_add_integer_vector_constant(
            state->llvmir_module, result_type, values, info->unit_count,
            &constant));
      }
      break;
    }
    case LOOM_LLVMIR_EMIT_CONST_KIND_FLOAT_BITS: {
      if (info->unit_count == 1) {
        IREE_RETURN_IF_ERROR(loom_llvmir_module_add_float_bits_constant(
            state->llvmir_module, result_type, (uint64_t)immediate, &constant));
      } else {
        loom_llvmir_type_id_t scalar_type = LOOM_LLVMIR_TYPE_ID_INVALID;
        IREE_RETURN_IF_ERROR(loom_llvmir_emit_core_scalar_type(
            state->llvmir_module, info->value_type,
            /*pointer_address_space=*/0, &scalar_type));
        loom_llvmir_value_id_t scalar_constant = LOOM_LLVMIR_VALUE_ID_INVALID;
        IREE_RETURN_IF_ERROR(loom_llvmir_module_add_float_bits_constant(
            state->llvmir_module, scalar_type, (uint64_t)immediate,
            &scalar_constant));
        IREE_RETURN_IF_ERROR(loom_llvmir_module_add_poison_constant(
            state->llvmir_module, result_type, &constant));
        for (uint32_t i = 0; i < info->unit_count; ++i) {
          loom_llvmir_value_id_t next = LOOM_LLVMIR_VALUE_ID_INVALID;
          IREE_RETURN_IF_ERROR(loom_llvmir_emit_insert_vector_lane(
              state, result_type, constant, scalar_constant, i,
              i + 1 == info->unit_count
                  ? loom_llvmir_emit_value_name(state->module, result_value)
                  : iree_string_view_empty(),
              &next));
          constant = next;
        }
      }
      break;
    }
  }

  loom_llvmir_emit_define_value(state, result_value, constant);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_splat(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  if (packet->op->operand_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 1);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const uint32_t lane_count =
      loom_llvmir_emit_low_value_unit_count(state, result_value);
  if (lane_count > LOOM_LLVMIR_MAX_VECTOR_LANES) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("vector_lane_count"), lane_count,
        LOOM_LLVMIR_MAX_VECTOR_LANES);
  }

  const loom_llvmir_value_id_t scalar = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  loom_llvmir_value_id_t current = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_poison_constant(
      state->llvmir_module, result_type, &current));
  for (uint32_t i = 0; i < lane_count; ++i) {
    loom_llvmir_value_id_t next = LOOM_LLVMIR_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_insert_vector_lane(
        state, result_type, current, scalar, i,
        i + 1 == lane_count
            ? loom_llvmir_emit_value_name(state->module, result_value)
            : iree_string_view_empty(),
        &next));
    current = next;
  }
  loom_llvmir_emit_define_value(state, result_value, current);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_from_elements(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const uint32_t lane_count =
      loom_llvmir_emit_low_value_unit_count(state, result_value);
  if (packet->op->operand_count != lane_count) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_operand"), packet->op->operand_count,
        lane_count);
  }
  if (lane_count > LOOM_LLVMIR_MAX_VECTOR_LANES) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("vector_lane_count"), lane_count,
        LOOM_LLVMIR_MAX_VECTOR_LANES);
  }

  loom_llvmir_value_id_t current = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_poison_constant(
      state->llvmir_module, result_type, &current));
  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  for (uint32_t i = 0; i < lane_count; ++i) {
    const loom_llvmir_value_id_t element =
        loom_llvmir_emit_lookup_value(state, operands[i]);
    loom_llvmir_value_id_t next = LOOM_LLVMIR_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_insert_vector_lane(
        state, result_type, current, element, i,
        i + 1 == lane_count
            ? loom_llvmir_emit_value_name(state->module, result_value)
            : iree_string_view_empty(),
        &next));
    current = next;
  }
  loom_llvmir_emit_define_value(state, result_value, current);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_concat(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_concat_info_t* info) {
  if (packet->op->operand_count != info->input_count) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_operand"), packet->op->operand_count,
        info->input_count);
  }
  loom_llvmir_type_id_t result_type_id = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type_id, &result_value));
  if (result_type_id == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  uint32_t result_lane_count = 0;
  loom_llvmir_type_id_t result_element_type_id = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_get_vector_type_info(
      state->llvmir_module, result_type_id, &result_lane_count,
      &result_element_type_id));

  loom_llvmir_value_id_t current = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_poison_constant(
      state->llvmir_module, result_type_id, &current));

  uint32_t result_lane = 0;
  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  for (uint32_t input_ordinal = 0; input_ordinal < info->input_count;
       ++input_ordinal) {
    const loom_value_id_t input_value = operands[input_ordinal];
    loom_llvmir_type_id_t input_type_id = LOOM_LLVMIR_TYPE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_type_for_low_value(
        state, packet->op, input_value, IREE_SV("input"),
        loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_OPERAND, input_ordinal),
        &input_type_id));
    if (input_type_id == LOOM_LLVMIR_TYPE_ID_INVALID) {
      return iree_ok_status();
    }
    uint32_t input_lane_count = 0;
    loom_llvmir_type_id_t input_element_type_id = LOOM_LLVMIR_TYPE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_module_get_vector_type_info(
        state->llvmir_module, input_type_id, &input_lane_count,
        &input_element_type_id));
    if (input_element_type_id != result_element_type_id) {
      return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                               IREE_SV("input_type"),
                                               input_type_id, result_type_id);
    }

    const loom_llvmir_value_id_t input =
        loom_llvmir_emit_lookup_value(state, input_value);
    for (uint32_t input_lane = 0; input_lane < input_lane_count;
         ++input_lane, ++result_lane) {
      if (result_lane >= result_lane_count) {
        return loom_llvmir_emit_shape_diagnostic(
            state, packet->op, IREE_SV("result_lane"), result_lane,
            result_lane_count);
      }
      loom_llvmir_value_id_t element = LOOM_LLVMIR_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_extract_vector_lane(
          state, result_element_type_id, input, input_lane,
          iree_string_view_empty(), &element));
      loom_llvmir_value_id_t next = LOOM_LLVMIR_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_insert_vector_lane(
          state, result_type_id, current, element, result_lane,
          result_lane + 1 == result_lane_count
              ? loom_llvmir_emit_value_name(state->module, result_value)
              : iree_string_view_empty(),
          &next));
      current = next;
    }
  }
  if (result_lane != result_lane_count) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("result_lane_count"),
                                             result_lane, result_lane_count);
  }
  loom_llvmir_emit_define_value(state, result_value, current);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_extract(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_emit_extract_kind_t kind) {
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  loom_llvmir_value_id_t index = LOOM_LLVMIR_VALUE_ID_INVALID;
  if (kind == LOOM_LLVMIR_EMIT_EXTRACT_KIND_DYNAMIC) {
    index = loom_llvmir_emit_lookup_value(state, operands[1]);
  } else {
    int64_t lane = 0;
    bool has_lane = false;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
        state, packet, IREE_SV("lane"), &has_lane, &lane));
    if (!has_lane) {
      return iree_ok_status();
    }
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_i64_constant(state, lane, &index));
  }
  const loom_llvmir_value_id_t source =
      loom_llvmir_emit_lookup_value(state, operands[0]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_extract_element(
      state->llvmir_block,
      &(loom_llvmir_extract_element_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .vector = source,
          .index = index,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_insert(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  if (packet->op->operand_count != 2) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 2);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  int64_t lane = 0;
  bool has_lane = false;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
      state, packet, IREE_SV("lane"), &has_lane, &lane));
  if (!has_lane) {
    return iree_ok_status();
  }

  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  const loom_llvmir_value_id_t dest =
      loom_llvmir_emit_lookup_value(state, operands[0]);
  const loom_llvmir_value_id_t value =
      loom_llvmir_emit_lookup_value(state, operands[1]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_insert_vector_lane(
      state, result_type, dest, value, (uint32_t)lane,
      loom_llvmir_emit_value_name(state->module, result_value),
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_dynamic_insert(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  if (packet->op->operand_count != 3) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 3);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  const loom_llvmir_value_id_t dest =
      loom_llvmir_emit_lookup_value(state, operands[0]);
  const loom_llvmir_value_id_t value =
      loom_llvmir_emit_lookup_value(state, operands[1]);
  const loom_llvmir_value_id_t index =
      loom_llvmir_emit_lookup_value(state, operands[2]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_insert_element(
      state->llvmir_block,
      &(loom_llvmir_insert_element_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .vector = dest,
          .element = value,
          .index = index,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_read_shuffle_mask(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet, uint32_t lane_count,
    uint64_t* out_lanes, bool* out_present) {
  *out_present = false;
  for (uint32_t i = 0; i < lane_count; ++i) {
    char lane_name_buffer[16] = {0};
    const int lane_name_length = snprintf(
        lane_name_buffer, IREE_ARRAYSIZE(lane_name_buffer), "lane%" PRIu32, i);
    if (lane_name_length < 0 || (iree_host_size_t)lane_name_length >=
                                    IREE_ARRAYSIZE(lane_name_buffer)) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "failed to format LLVMIR shuffle lane name");
    }
    int64_t lane = 0;
    bool has_lane = false;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
        state, packet,
        iree_make_string_view(lane_name_buffer,
                              (iree_host_size_t)lane_name_length),
        &has_lane, &lane));
    if (!has_lane) {
      return iree_ok_status();
    }
    out_lanes[i] = (uint64_t)lane;
  }
  *out_present = true;
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_shuffle_like(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    loom_llvmir_emit_shuffle_kind_t kind) {
  if (packet->op->operand_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 1);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const uint32_t lane_count =
      loom_llvmir_emit_low_value_unit_count(state, result_value);
  if (lane_count > LOOM_LLVMIR_MAX_VECTOR_LANES) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("vector_lane_count"), lane_count,
        LOOM_LLVMIR_MAX_VECTOR_LANES);
  }

  uint64_t lanes[LOOM_LLVMIR_MAX_VECTOR_LANES] = {0};
  switch (kind) {
    case LOOM_LLVMIR_EMIT_SHUFFLE_KIND_EXPLICIT: {
      bool has_lanes = false;
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_shuffle_mask(
          state, packet, lane_count, lanes, &has_lanes));
      if (!has_lanes) {
        return iree_ok_status();
      }
      break;
    }
    case LOOM_LLVMIR_EMIT_SHUFFLE_KIND_SLICE: {
      int64_t offset = 0;
      bool has_offset = false;
      IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
          state, packet, IREE_SV("offset"), &has_offset, &offset));
      if (!has_offset) {
        return iree_ok_status();
      }
      for (uint32_t i = 0; i < lane_count; ++i) {
        lanes[i] = (uint64_t)(offset + i);
      }
      break;
    }
  }

  const loom_value_id_t source_value = loom_op_const_operands(packet->op)[0];
  loom_llvmir_type_id_t source_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_type_for_low_value(
      state, packet->op, source_value, IREE_SV("source"),
      loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_OPERAND, 0),
      &source_type));
  if (source_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
    return iree_ok_status();
  }

  const loom_llvmir_value_id_t source =
      loom_llvmir_emit_lookup_value(state, source_value);
  loom_llvmir_value_id_t poison = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_poison_constant(
      state->llvmir_module, source_type, &poison));

  loom_llvmir_type_id_t i32_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_module_get_integer_type(state->llvmir_module, 32, &i32_type));
  loom_llvmir_type_id_t mask_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_get_vector_type(
      state->llvmir_module, lane_count, i32_type, &mask_type));
  loom_llvmir_value_id_t mask = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_integer_vector_constant(
      state->llvmir_module, mask_type, lanes, lane_count, &mask));

  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_shuffle_vector(
      state->llvmir_block,
      &(loom_llvmir_shuffle_vector_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .lhs = source,
          .rhs = poison,
          .mask = mask,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_constant_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet) {
  const loom_llvmir_emit_const_info_t* info =
      loom_llvmir_emit_lookup_const(packet->descriptor_ordinal);
  return info ? loom_llvmir_emit_const(state, packet, info)
              : loom_llvmir_emit_unsupported_descriptor_diagnostic(state,
                                                                   packet);
}

iree_status_t loom_llvmir_emit_vector_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet, bool* out_matched) {
  *out_matched = true;
  const uint32_t descriptor_ref = packet->descriptor_ordinal;

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kSplatDescriptorRefs,
          IREE_ARRAYSIZE(kSplatDescriptorRefs))) {
    return loom_llvmir_emit_splat(state, packet);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kFromElementsDescriptorRefs,
          IREE_ARRAYSIZE(kFromElementsDescriptorRefs))) {
    return loom_llvmir_emit_from_elements(state, packet);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kExtractDescriptorRefs,
          IREE_ARRAYSIZE(kExtractDescriptorRefs))) {
    return loom_llvmir_emit_extract(state, packet,
                                    LOOM_LLVMIR_EMIT_EXTRACT_KIND_STATIC);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kDynamicExtractDescriptorRefs,
          IREE_ARRAYSIZE(kDynamicExtractDescriptorRefs))) {
    return loom_llvmir_emit_extract(state, packet,
                                    LOOM_LLVMIR_EMIT_EXTRACT_KIND_DYNAMIC);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kInsertDescriptorRefs,
          IREE_ARRAYSIZE(kInsertDescriptorRefs))) {
    return loom_llvmir_emit_insert(state, packet);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kDynamicInsertDescriptorRefs,
          IREE_ARRAYSIZE(kDynamicInsertDescriptorRefs))) {
    return loom_llvmir_emit_dynamic_insert(state, packet);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kShuffleDescriptorRefs,
          IREE_ARRAYSIZE(kShuffleDescriptorRefs))) {
    return loom_llvmir_emit_shuffle_like(
        state, packet, LOOM_LLVMIR_EMIT_SHUFFLE_KIND_EXPLICIT);
  }

  const loom_llvmir_emit_concat_info_t* concat_info =
      loom_llvmir_emit_lookup_concat(descriptor_ref);
  if (concat_info) {
    return loom_llvmir_emit_concat(state, packet, concat_info);
  }

  if (loom_llvmir_emit_descriptor_ref_in(
          descriptor_ref, kSliceDescriptorRefs,
          IREE_ARRAYSIZE(kSliceDescriptorRefs))) {
    return loom_llvmir_emit_shuffle_like(state, packet,
                                         LOOM_LLVMIR_EMIT_SHUFFLE_KIND_SLICE);
  }

  *out_matched = false;
  return iree_ok_status();
}
