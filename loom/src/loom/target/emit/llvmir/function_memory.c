// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/function_memory.h"

#include "loom/ir/facts.h"
#include "loom/ops/atomic.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/llvmir/descriptors/descriptors.h"
#include "loom/target/registers.h"

typedef enum loom_llvmir_emit_memory_flag_bits_e {
  LOOM_LLVMIR_EMIT_MEMORY_FLAG_LOAD = 1u << 0,
  LOOM_LLVMIR_EMIT_MEMORY_FLAG_INDEXED = 1u << 1,
} loom_llvmir_emit_memory_flag_bits_t;
typedef uint8_t loom_llvmir_emit_memory_flags_t;

typedef enum loom_llvmir_emit_atomic_flag_bits_e {
  LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE = 1u << 0,
  LOOM_LLVMIR_EMIT_ATOMIC_FLAG_INDEXED = 1u << 1,
  LOOM_LLVMIR_EMIT_ATOMIC_FLAG_CMPXCHG = 1u << 2,
} loom_llvmir_emit_atomic_flag_bits_t;
typedef uint8_t loom_llvmir_emit_atomic_flags_t;

typedef struct loom_llvmir_emit_memory_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // Loaded or stored value type family.
  loom_llvmir_emit_core_type_t value_type;
  // Number of vector lanes in the loaded or stored value.
  uint32_t unit_count;
  // Memory operation flags selecting load/store and indexed addressing.
  loom_llvmir_emit_memory_flags_t flags;
} loom_llvmir_emit_memory_info_t;

typedef struct loom_llvmir_emit_atomic_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // Scalar value type used by the atomic operation.
  loom_llvmir_emit_core_type_t value_type;
  // LLVMIR builder atomic read-modify-write opcode for non-cmpxchg forms.
  loom_llvmir_atomic_rmw_op_t op;
  // Atomic operation flags selecting result and indexed addressing forms.
  loom_llvmir_emit_atomic_flags_t flags;
} loom_llvmir_emit_atomic_info_t;

typedef struct loom_llvmir_emit_alloca_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // Source scratch memory space represented by the descriptor.
  loom_value_fact_memory_space_t memory_space;
} loom_llvmir_emit_alloca_info_t;

#define LOOM_LLVMIR_MEMORY_INFO(ref, type, units, flags) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##ref, type, units, flags}

#define LOOM_LLVMIR_MEMORY_INFOS(suffix, type, units)                    \
  LOOM_LLVMIR_MEMORY_INFO(LOAD_##suffix, type, units,                    \
                          LOOM_LLVMIR_EMIT_MEMORY_FLAG_LOAD),            \
      LOOM_LLVMIR_MEMORY_INFO(STORE_##suffix, type, units, 0),           \
      LOOM_LLVMIR_MEMORY_INFO(LOAD_INDEXED_##suffix, type, units,        \
                              LOOM_LLVMIR_EMIT_MEMORY_FLAG_LOAD |        \
                                  LOOM_LLVMIR_EMIT_MEMORY_FLAG_INDEXED), \
      LOOM_LLVMIR_MEMORY_INFO(STORE_INDEXED_##suffix, type, units,       \
                              LOOM_LLVMIR_EMIT_MEMORY_FLAG_INDEXED)

#define LOOM_LLVMIR_VECTOR_MEMORY_INFOS(lanes, suffix, type) \
  LOOM_LLVMIR_MEMORY_INFOS(V##lanes##suffix, type, lanes)

#define LOOM_LLVMIR_ALL_MEMORY_INFOS(suffix, type)      \
  LOOM_LLVMIR_MEMORY_INFOS(suffix, type, 1),            \
      LOOM_LLVMIR_VECTOR_MEMORY_INFOS(2, suffix, type), \
      LOOM_LLVMIR_VECTOR_MEMORY_INFOS(3, suffix, type), \
      LOOM_LLVMIR_VECTOR_MEMORY_INFOS(4, suffix, type), \
      LOOM_LLVMIR_VECTOR_MEMORY_INFOS(8, suffix, type), \
      LOOM_LLVMIR_VECTOR_MEMORY_INFOS(16, suffix, type)

static const loom_llvmir_emit_memory_info_t kMemoryInfos[] = {
    LOOM_LLVMIR_ALL_MEMORY_INFOS(I8, LOOM_LLVMIR_EMIT_CORE_TYPE_I8),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(I16, LOOM_LLVMIR_EMIT_CORE_TYPE_I16),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(I32, LOOM_LLVMIR_EMIT_CORE_TYPE_I32),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(I64, LOOM_LLVMIR_EMIT_CORE_TYPE_I64),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(F16, LOOM_LLVMIR_EMIT_CORE_TYPE_F16),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(BF16, LOOM_LLVMIR_EMIT_CORE_TYPE_BF16),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(F32, LOOM_LLVMIR_EMIT_CORE_TYPE_F32),
    LOOM_LLVMIR_ALL_MEMORY_INFOS(F64, LOOM_LLVMIR_EMIT_CORE_TYPE_F64),
};

#undef LOOM_LLVMIR_ALL_MEMORY_INFOS
#undef LOOM_LLVMIR_VECTOR_MEMORY_INFOS
#undef LOOM_LLVMIR_MEMORY_INFOS
#undef LOOM_LLVMIR_MEMORY_INFO

#define LOOM_LLVMIR_ATOMIC_INFO(form, op_suffix, type_suffix, type, op, flags)              \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_ATOMIC_##form##_##op_suffix##_##type_suffix,          \
   type, op, flags},                                                                        \
  {                                                                                         \
    LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_ATOMIC_##form##_INDEXED_##op_suffix##_##type_suffix, \
        type, op, flags | LOOM_LLVMIR_EMIT_ATOMIC_FLAG_INDEXED                              \
  }

#define LOOM_LLVMIR_ATOMIC_CMPXCHG_INFO(type_suffix, type)                   \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_ATOMIC_CMPXCHG_##type_suffix, type,    \
   LOOM_LLVMIR_ATOMIC_RMW_XCHG,                                              \
   LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE |                               \
       LOOM_LLVMIR_EMIT_ATOMIC_FLAG_CMPXCHG},                                \
  {                                                                          \
    LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_ATOMIC_CMPXCHG_INDEXED_##type_suffix, \
        type, LOOM_LLVMIR_ATOMIC_RMW_XCHG,                                   \
        LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE |                          \
            LOOM_LLVMIR_EMIT_ATOMIC_FLAG_CMPXCHG |                           \
            LOOM_LLVMIR_EMIT_ATOMIC_FLAG_INDEXED                             \
  }

#define LOOM_LLVMIR_INTEGER_ATOMIC_REDUCE_INFOS(type_suffix, type) \
  LOOM_LLVMIR_ATOMIC_INFO(REDUCE, ADD, type_suffix, type,          \
                          LOOM_LLVMIR_ATOMIC_RMW_ADD, 0),          \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, SUB, type_suffix, type,      \
                              LOOM_LLVMIR_ATOMIC_RMW_SUB, 0),      \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, AND, type_suffix, type,      \
                              LOOM_LLVMIR_ATOMIC_RMW_AND, 0),      \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, OR, type_suffix, type,       \
                              LOOM_LLVMIR_ATOMIC_RMW_OR, 0),       \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, XOR, type_suffix, type,      \
                              LOOM_LLVMIR_ATOMIC_RMW_XOR, 0),      \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, MIN, type_suffix, type,      \
                              LOOM_LLVMIR_ATOMIC_RMW_MIN, 0),      \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, MAX, type_suffix, type,      \
                              LOOM_LLVMIR_ATOMIC_RMW_MAX, 0),      \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, UMIN, type_suffix, type,     \
                              LOOM_LLVMIR_ATOMIC_RMW_UMIN, 0),     \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, UMAX, type_suffix, type,     \
                              LOOM_LLVMIR_ATOMIC_RMW_UMAX, 0)

#define LOOM_LLVMIR_INTEGER_ATOMIC_RMW_INFOS(type_suffix, type)           \
  LOOM_LLVMIR_ATOMIC_INFO(RMW, XCHG, type_suffix, type,                   \
                          LOOM_LLVMIR_ATOMIC_RMW_XCHG,                    \
                          LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE),     \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, ADD, type_suffix, type,                \
                              LOOM_LLVMIR_ATOMIC_RMW_ADD,                 \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, SUB, type_suffix, type,                \
                              LOOM_LLVMIR_ATOMIC_RMW_SUB,                 \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, AND, type_suffix, type,                \
                              LOOM_LLVMIR_ATOMIC_RMW_AND,                 \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, OR, type_suffix, type,                 \
                              LOOM_LLVMIR_ATOMIC_RMW_OR,                  \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, XOR, type_suffix, type,                \
                              LOOM_LLVMIR_ATOMIC_RMW_XOR,                 \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, MIN, type_suffix, type,                \
                              LOOM_LLVMIR_ATOMIC_RMW_MIN,                 \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, MAX, type_suffix, type,                \
                              LOOM_LLVMIR_ATOMIC_RMW_MAX,                 \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, UMIN, type_suffix, type,               \
                              LOOM_LLVMIR_ATOMIC_RMW_UMIN,                \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, UMAX, type_suffix, type,               \
                              LOOM_LLVMIR_ATOMIC_RMW_UMAX,                \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE)

#define LOOM_LLVMIR_FLOAT_ATOMIC_REDUCE_INFOS(type_suffix, type)   \
  LOOM_LLVMIR_ATOMIC_INFO(REDUCE, FADD, type_suffix, type,         \
                          LOOM_LLVMIR_ATOMIC_RMW_FADD, 0),         \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, FMINIMUM, type_suffix, type, \
                              LOOM_LLVMIR_ATOMIC_RMW_FMINIMUM, 0), \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, FMAXIMUM, type_suffix, type, \
                              LOOM_LLVMIR_ATOMIC_RMW_FMAXIMUM, 0), \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, FMIN, type_suffix, type,     \
                              LOOM_LLVMIR_ATOMIC_RMW_FMIN, 0),     \
      LOOM_LLVMIR_ATOMIC_INFO(REDUCE, FMAX, type_suffix, type,     \
                              LOOM_LLVMIR_ATOMIC_RMW_FMAX, 0)

#define LOOM_LLVMIR_FLOAT_ATOMIC_RMW_INFOS(type_suffix, type)             \
  LOOM_LLVMIR_ATOMIC_INFO(RMW, XCHG, type_suffix, type,                   \
                          LOOM_LLVMIR_ATOMIC_RMW_XCHG,                    \
                          LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE),     \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, FADD, type_suffix, type,               \
                              LOOM_LLVMIR_ATOMIC_RMW_FADD,                \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, FMINIMUM, type_suffix, type,           \
                              LOOM_LLVMIR_ATOMIC_RMW_FMINIMUM,            \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, FMAXIMUM, type_suffix, type,           \
                              LOOM_LLVMIR_ATOMIC_RMW_FMAXIMUM,            \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, FMIN, type_suffix, type,               \
                              LOOM_LLVMIR_ATOMIC_RMW_FMIN,                \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE), \
      LOOM_LLVMIR_ATOMIC_INFO(RMW, FMAX, type_suffix, type,               \
                              LOOM_LLVMIR_ATOMIC_RMW_FMAX,                \
                              LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE)

#define LOOM_LLVMIR_INTEGER_ATOMIC_INFOS(type_suffix, type)    \
  LOOM_LLVMIR_INTEGER_ATOMIC_REDUCE_INFOS(type_suffix, type),  \
      LOOM_LLVMIR_INTEGER_ATOMIC_RMW_INFOS(type_suffix, type), \
      LOOM_LLVMIR_ATOMIC_CMPXCHG_INFO(type_suffix, type)

#define LOOM_LLVMIR_FLOAT_ATOMIC_INFOS(type_suffix, type)   \
  LOOM_LLVMIR_FLOAT_ATOMIC_REDUCE_INFOS(type_suffix, type), \
      LOOM_LLVMIR_FLOAT_ATOMIC_RMW_INFOS(type_suffix, type)

static const loom_llvmir_emit_atomic_info_t kAtomicInfos[] = {
    LOOM_LLVMIR_INTEGER_ATOMIC_INFOS(I8, LOOM_LLVMIR_EMIT_CORE_TYPE_I8),
    LOOM_LLVMIR_INTEGER_ATOMIC_INFOS(I16, LOOM_LLVMIR_EMIT_CORE_TYPE_I16),
    LOOM_LLVMIR_INTEGER_ATOMIC_INFOS(I32, LOOM_LLVMIR_EMIT_CORE_TYPE_I32),
    LOOM_LLVMIR_INTEGER_ATOMIC_INFOS(I64, LOOM_LLVMIR_EMIT_CORE_TYPE_I64),
    LOOM_LLVMIR_FLOAT_ATOMIC_INFOS(F32, LOOM_LLVMIR_EMIT_CORE_TYPE_F32),
    LOOM_LLVMIR_FLOAT_ATOMIC_INFOS(F64, LOOM_LLVMIR_EMIT_CORE_TYPE_F64),
};

#undef LOOM_LLVMIR_FLOAT_ATOMIC_INFOS
#undef LOOM_LLVMIR_INTEGER_ATOMIC_INFOS
#undef LOOM_LLVMIR_FLOAT_ATOMIC_RMW_INFOS
#undef LOOM_LLVMIR_FLOAT_ATOMIC_REDUCE_INFOS
#undef LOOM_LLVMIR_INTEGER_ATOMIC_RMW_INFOS
#undef LOOM_LLVMIR_INTEGER_ATOMIC_REDUCE_INFOS
#undef LOOM_LLVMIR_ATOMIC_CMPXCHG_INFO
#undef LOOM_LLVMIR_ATOMIC_INFO

static const loom_llvmir_emit_alloca_info_t kAllocaInfos[] = {
    {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_ALLOCA_PRIVATE_I8,
     LOOM_VALUE_FACT_MEMORY_SPACE_PRIVATE},
    {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_ALLOCA_WORKGROUP_I8,
     LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP},
};

static bool loom_llvmir_emit_core_type_byte_count(
    loom_llvmir_emit_core_type_t type, uint32_t* out_byte_count) {
  switch (type) {
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I1:
      *out_byte_count = 1;
      return true;
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I8:
      *out_byte_count = 1;
      return true;
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I16:
    case LOOM_LLVMIR_EMIT_CORE_TYPE_F16:
    case LOOM_LLVMIR_EMIT_CORE_TYPE_BF16:
      *out_byte_count = 2;
      return true;
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I32:
    case LOOM_LLVMIR_EMIT_CORE_TYPE_F32:
      *out_byte_count = 4;
      return true;
    case LOOM_LLVMIR_EMIT_CORE_TYPE_I64:
    case LOOM_LLVMIR_EMIT_CORE_TYPE_F64:
      *out_byte_count = 8;
      return true;
    case LOOM_LLVMIR_EMIT_CORE_TYPE_PTR:
      return false;
  }
  return false;
}

static const loom_llvmir_emit_alloca_info_t* loom_llvmir_emit_lookup_alloca(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kAllocaInfos); ++i) {
    if (kAllocaInfos[i].descriptor_ref == descriptor_ref) {
      return &kAllocaInfos[i];
    }
  }
  return NULL;
}

static bool loom_llvmir_emit_memory_space_address_space(
    const loom_llvmir_target_profile_t* profile,
    loom_value_fact_memory_space_t memory_space, uint32_t* out_address_space) {
  switch (memory_space) {
    case LOOM_VALUE_FACT_MEMORY_SPACE_UNKNOWN:
    case LOOM_VALUE_FACT_MEMORY_SPACE_GENERIC:
      *out_address_space = profile->target_env->address_spaces.generic;
      return true;
    case LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL:
      *out_address_space = profile->target_env->address_spaces.global;
      return true;
    case LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP:
      *out_address_space = profile->target_env->address_spaces.local;
      return true;
    case LOOM_VALUE_FACT_MEMORY_SPACE_PRIVATE:
      *out_address_space = profile->target_env->address_spaces.private_memory;
      return true;
    case LOOM_VALUE_FACT_MEMORY_SPACE_CONSTANT:
      *out_address_space = profile->target_env->address_spaces.constant;
      return true;
    case LOOM_VALUE_FACT_MEMORY_SPACE_DESCRIPTOR:
      if (profile->target_env->address_spaces.buffer_resource == UINT32_MAX) {
        return false;
      }
      *out_address_space = profile->target_env->address_spaces.buffer_resource;
      return true;
    case LOOM_VALUE_FACT_MEMORY_SPACE_HOST:
      return false;
  }
  return false;
}

iree_status_t loom_llvmir_emit_pointer_address_space_for_low_value(
    loom_llvmir_emit_function_state_t* state, const loom_op_t* op,
    loom_value_id_t value_id, iree_string_view_t value_kind,
    iree_string_view_t expected_constraint, uint32_t* out_address_space,
    bool* out_supported) {
  *out_address_space =
      state->target_profile->target_env->address_spaces.generic;
  *out_supported = true;
  if (!loom_llvmir_emit_low_value_is_pointer_register(state, value_id)) {
    *out_supported = false;
    return loom_llvmir_emit_value_type_diagnostic(
        state, op, value_id, value_kind, expected_constraint);
  }

  const loom_value_t* value = loom_module_value(state->module, value_id);
  if (loom_value_is_block_arg(value)) {
    // Entry pointers have an ABI address space. A transported ptr register
    // carries no address-space identity for its incoming resource or scratch.
    if (loom_value_def_block(value)->region_index != 0) {
      *out_supported = false;
      return loom_llvmir_emit_value_type_diagnostic(
          state, op, value_id, value_kind,
          IREE_SV("non-pointer block argument"));
    }
    if (state->target_profile->kind == LOOM_LLVMIR_TARGET_PROFILE_KERNEL) {
      *out_supported = false;
      return loom_llvmir_emit_value_type_diagnostic(
          state, op, value_id, value_kind,
          IREE_SV("low.resource<hal_binding> pointer"));
    }
    return iree_ok_status();
  }

  const loom_op_t* def_op = loom_value_def_op(value);
  if (def_op != NULL && loom_low_resource_isa(def_op)) {
    switch (loom_low_resource_import_kind(def_op)) {
      case LOOM_LOW_RESOURCE_IMPORT_KIND_NATIVE_POINTER:
        *out_address_space =
            state->target_profile->target_env->address_spaces.generic;
        return iree_ok_status();
      case LOOM_LOW_RESOURCE_IMPORT_KIND_HAL_BINDING:
        *out_address_space =
            state->target_profile->target_env->address_spaces.global;
        return iree_ok_status();
      default:
        *out_supported = false;
        return loom_llvmir_emit_value_type_diagnostic(
            state, op, value_id, value_kind,
            IREE_SV("native_pointer or hal_binding pointer resource"));
    }
  }
  if (def_op != NULL) {
    loom_low_descriptor_packet_t packet = {0};
    loom_low_descriptor_packet_initialize(state->target->descriptor_set, def_op,
                                          &packet);
    if (packet.kind != LOOM_LOW_DESCRIPTOR_PACKET_NONE) {
      const loom_llvmir_emit_alloca_info_t* alloca_info =
          loom_llvmir_emit_lookup_alloca(packet.descriptor_ordinal);
      if (alloca_info != NULL &&
          !loom_llvmir_emit_memory_space_address_space(
              state->target_profile, alloca_info->memory_space,
              out_address_space)) {
        *out_supported = false;
        return loom_llvmir_emit_value_type_diagnostic(
            state, op, value_id, value_kind,
            IREE_SV("LLVMIR-addressable scratch pointer"));
      }
    }
  }
  return iree_ok_status();
}

static const loom_llvmir_emit_memory_info_t* loom_llvmir_emit_lookup_memory(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kMemoryInfos); ++i) {
    if (kMemoryInfos[i].descriptor_ref == descriptor_ref) {
      return &kMemoryInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_atomic_info_t* loom_llvmir_emit_lookup_atomic(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kAtomicInfos); ++i) {
    if (kAtomicInfos[i].descriptor_ref == descriptor_ref) {
      return &kAtomicInfos[i];
    }
  }
  return NULL;
}

static iree_status_t loom_llvmir_emit_byte_pointer(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_memory_info_t* info, uint32_t pointer_operand_index,
    loom_llvmir_value_id_t base, loom_llvmir_value_id_t* out_pointer) {
  int64_t byte_offset = 0;
  bool has_byte_offset = false;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
      state, packet, IREE_SV("byte_offset"), &has_byte_offset, &byte_offset));
  if (!has_byte_offset) {
    *out_pointer = LOOM_LLVMIR_VALUE_ID_INVALID;
    return iree_ok_status();
  }

  const bool indexed =
      iree_any_bit_set(info->flags, LOOM_LLVMIR_EMIT_MEMORY_FLAG_INDEXED);
  if (!indexed && byte_offset == 0) {
    *out_pointer = base;
    return iree_ok_status();
  }

  loom_llvmir_value_id_t byte_index = LOOM_LLVMIR_VALUE_ID_INVALID;
  if (indexed) {
    int64_t byte_stride = 0;
    bool has_byte_stride = false;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
        state, packet, IREE_SV("byte_stride"), &has_byte_stride, &byte_stride));
    if (!has_byte_stride) {
      *out_pointer = LOOM_LLVMIR_VALUE_ID_INVALID;
      return iree_ok_status();
    }

    loom_llvmir_value_id_t index = LOOM_LLVMIR_VALUE_ID_INVALID;
    // Indexed memory descriptors place the index immediately after the pointer.
    // Compare-exchange has two value operands before that pointer.
    const uint32_t index_operand_index = pointer_operand_index + 1;
    index = loom_llvmir_emit_lookup_value(
        state, loom_op_const_operands(packet->op)[index_operand_index]);
    if (byte_stride == 1) {
      byte_index = index;
    } else {
      loom_llvmir_type_id_t i64_type = LOOM_LLVMIR_TYPE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_llvmir_module_get_integer_type(
          state->llvmir_module, 64, &i64_type));
      loom_llvmir_value_id_t stride_value = LOOM_LLVMIR_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(
          loom_llvmir_emit_i64_constant(state, byte_stride, &stride_value));
      IREE_RETURN_IF_ERROR(
          loom_llvmir_build_binop(state->llvmir_block,
                                  &(loom_llvmir_binop_desc_t){
                                      .result_type = i64_type,
                                      .op = LOOM_LLVMIR_BINOP_MUL,
                                      .lhs = index,
                                      .rhs = stride_value,
                                  },
                                  &byte_index));
    }

    if (byte_offset != 0) {
      loom_llvmir_type_id_t i64_type = LOOM_LLVMIR_TYPE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_llvmir_module_get_integer_type(
          state->llvmir_module, 64, &i64_type));
      loom_llvmir_value_id_t offset_value = LOOM_LLVMIR_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(
          loom_llvmir_emit_i64_constant(state, byte_offset, &offset_value));
      loom_llvmir_value_id_t indexed_offset = LOOM_LLVMIR_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(
          loom_llvmir_build_binop(state->llvmir_block,
                                  &(loom_llvmir_binop_desc_t){
                                      .result_type = i64_type,
                                      .op = LOOM_LLVMIR_BINOP_ADD,
                                      .lhs = byte_index,
                                      .rhs = offset_value,
                                  },
                                  &indexed_offset));
      byte_index = indexed_offset;
    }
  } else {
    IREE_RETURN_IF_ERROR(
        loom_llvmir_emit_i64_constant(state, byte_offset, &byte_index));
  }

  loom_llvmir_type_id_t i8_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_llvmir_type_id_t ptr_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_module_get_integer_type(state->llvmir_module, 8, &i8_type));
  uint32_t pointer_address_space = 0;
  bool supported = true;
  const loom_value_id_t base_value_id =
      loom_op_const_operands(packet->op)[pointer_operand_index];
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_pointer_address_space_for_low_value(
      state, packet->op, base_value_id, IREE_SV("memory_pointer"),
      IREE_SV("reg<llvmir.ptr> memory pointer"), &pointer_address_space,
      &supported));
  if (!supported) {
    *out_pointer = LOOM_LLVMIR_VALUE_ID_INVALID;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_llvmir_module_get_pointer_type(
      state->llvmir_module, pointer_address_space, &ptr_type));
  return loom_llvmir_build_gep(state->llvmir_block,
                               &(loom_llvmir_gep_desc_t){
                                   .result_type = ptr_type,
                                   .element_type = i8_type,
                                   .base = base,
                                   .indices = &byte_index,
                                   .index_count = 1,
                               },
                               out_pointer);
}

static iree_status_t loom_llvmir_emit_alloca(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_alloca_info_t* info) {
  if (packet->op->operand_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 1);
  }
  if (packet->op->result_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_result"),
                                             packet->op->result_count, 1);
  }

  int64_t base_alignment = 0;
  bool has_base_alignment = false;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
      state, packet, IREE_SV("base_alignment"), &has_base_alignment,
      &base_alignment));
  if (!has_base_alignment) {
    return iree_ok_status();
  }

  uint32_t pointer_address_space = 0;
  if (!loom_llvmir_emit_memory_space_address_space(
          state->target_profile, info->memory_space, &pointer_address_space)) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("memory_space_addressable"), 0, 1);
  }

  loom_llvmir_type_id_t i8_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_module_get_integer_type(state->llvmir_module, 8, &i8_type));
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_module_get_pointer_type(
      state->llvmir_module, pointer_address_space, &result_type));

  const loom_llvmir_value_id_t byte_length = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);

  const loom_value_id_t result_value = loom_op_const_results(packet->op)[0];
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_alloca(
      state->llvmir_block,
      &(loom_llvmir_alloca_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .element_type = i8_type,
          .count = byte_length,
          .alignment = (uint32_t)base_alignment,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static bool loom_llvmir_emit_atomic_ordering(
    int64_t ordering, loom_llvmir_atomic_ordering_t* out_ordering) {
  switch ((loom_atomic_ordering_t)ordering) {
    case LOOM_ATOMIC_ORDERING_RELAXED:
      *out_ordering = LOOM_LLVMIR_ATOMIC_ORDERING_MONOTONIC;
      return true;
    case LOOM_ATOMIC_ORDERING_ACQUIRE:
      *out_ordering = LOOM_LLVMIR_ATOMIC_ORDERING_ACQUIRE;
      return true;
    case LOOM_ATOMIC_ORDERING_RELEASE:
      *out_ordering = LOOM_LLVMIR_ATOMIC_ORDERING_RELEASE;
      return true;
    case LOOM_ATOMIC_ORDERING_ACQ_REL:
      *out_ordering = LOOM_LLVMIR_ATOMIC_ORDERING_ACQ_REL;
      return true;
    case LOOM_ATOMIC_ORDERING_SEQ_CST:
      *out_ordering = LOOM_LLVMIR_ATOMIC_ORDERING_SEQ_CST;
      return true;
    case LOOM_ATOMIC_ORDERING_COUNT_:
      return false;
  }
  return false;
}

static bool loom_llvmir_emit_atomic_sync_scope(
    int64_t scope, iree_string_view_t* out_sync_scope) {
  switch ((loom_atomic_scope_t)scope) {
    case LOOM_ATOMIC_SCOPE_THREAD:
      *out_sync_scope = IREE_SV("singlethread");
      return true;
    case LOOM_ATOMIC_SCOPE_SUBGROUP:
      *out_sync_scope = IREE_SV("subgroup");
      return true;
    case LOOM_ATOMIC_SCOPE_WORKGROUP:
      *out_sync_scope = IREE_SV("workgroup");
      return true;
    case LOOM_ATOMIC_SCOPE_DEVICE:
    case LOOM_ATOMIC_SCOPE_SYSTEM:
      *out_sync_scope = iree_string_view_empty();
      return true;
    case LOOM_ATOMIC_SCOPE_COUNT_:
      return false;
  }
  return false;
}

static iree_status_t loom_llvmir_emit_memory(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_memory_info_t* info) {
  const bool is_load =
      iree_any_bit_set(info->flags, LOOM_LLVMIR_EMIT_MEMORY_FLAG_LOAD);
  const uint32_t expected_operands =
      (is_load ? 1u : 2u) +
      (iree_any_bit_set(info->flags, LOOM_LLVMIR_EMIT_MEMORY_FLAG_INDEXED)
           ? 1u
           : 0u);
  if (packet->op->operand_count != expected_operands) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_operand"), packet->op->operand_count,
        expected_operands);
  }
  if (packet->op->result_count != (is_load ? 1u : 0u)) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_result"), packet->op->result_count,
        is_load ? 1u : 0u);
  }

  const uint32_t pointer_operand_index = is_load ? 0 : 1;
  const loom_value_id_t base_value_id =
      loom_op_const_operands(packet->op)[pointer_operand_index];
  const loom_llvmir_value_id_t base =
      loom_llvmir_emit_lookup_value(state, base_value_id);
  loom_llvmir_value_id_t pointer = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_byte_pointer(
      state, packet, info, pointer_operand_index, base, &pointer));
  if (pointer == LOOM_LLVMIR_VALUE_ID_INVALID) {
    return iree_ok_status();
  }

  if (is_load) {
    loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
    loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
        state, packet, &result_type, &result_value));
    if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) {
      return iree_ok_status();
    }

    loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_build_load(
        state->llvmir_block,
        &(loom_llvmir_load_desc_t){
            .result_name =
                loom_llvmir_emit_value_name(state->module, result_value),
            .result_type = result_type,
            .pointer = pointer,
        },
        &llvmir_result));
    loom_llvmir_emit_define_value(state, result_value, llvmir_result);
    return iree_ok_status();
  }

  loom_llvmir_type_id_t value_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_core_type(
      state->llvmir_module, info->value_type, info->unit_count,
      /*pointer_address_space=*/0, &value_type));
  const loom_llvmir_value_id_t value = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  return loom_llvmir_build_store(state->llvmir_block,
                                 &(loom_llvmir_store_desc_t){
                                     .value = value,
                                     .pointer = pointer,
                                 });
}

static iree_status_t loom_llvmir_emit_atomic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_atomic_info_t* info) {
  const bool indexed =
      iree_any_bit_set(info->flags, LOOM_LLVMIR_EMIT_ATOMIC_FLAG_INDEXED);
  const bool is_cmpxchg =
      iree_any_bit_set(info->flags, LOOM_LLVMIR_EMIT_ATOMIC_FLAG_CMPXCHG);
  const bool returns_value =
      iree_any_bit_set(info->flags, LOOM_LLVMIR_EMIT_ATOMIC_FLAG_RETURN_VALUE);
  const uint32_t expected_operands =
      (is_cmpxchg ? 3u : 2u) + (indexed ? 1u : 0u);
  if (packet->op->operand_count != expected_operands) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_operand"), packet->op->operand_count,
        expected_operands);
  }
  if (packet->op->result_count != (returns_value ? 1u : 0u)) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("packet_result"), packet->op->result_count,
        returns_value ? 1u : 0u);
  }

  loom_llvmir_atomic_ordering_t success_ordering =
      LOOM_LLVMIR_ATOMIC_ORDERING_MONOTONIC;
  loom_llvmir_atomic_ordering_t failure_ordering =
      LOOM_LLVMIR_ATOMIC_ORDERING_MONOTONIC;
  if (is_cmpxchg) {
    int64_t success_ordering_attr = 0;
    bool has_success_ordering = false;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
        state, packet, IREE_SV("success_ordering"), &has_success_ordering,
        &success_ordering_attr));
    if (!has_success_ordering) {
      return iree_ok_status();
    }
    if (!loom_llvmir_emit_atomic_ordering(success_ordering_attr,
                                          &success_ordering)) {
      return loom_llvmir_emit_shape_diagnostic(
          state, packet->op, IREE_SV("atomic_success_ordering"),
          (uint32_t)success_ordering_attr, LOOM_ATOMIC_ORDERING_COUNT_);
    }
    int64_t failure_ordering_attr = 0;
    bool has_failure_ordering = false;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
        state, packet, IREE_SV("failure_ordering"), &has_failure_ordering,
        &failure_ordering_attr));
    if (!has_failure_ordering) {
      return iree_ok_status();
    }
    if (!loom_llvmir_emit_atomic_ordering(failure_ordering_attr,
                                          &failure_ordering)) {
      return loom_llvmir_emit_shape_diagnostic(
          state, packet->op, IREE_SV("atomic_failure_ordering"),
          (uint32_t)failure_ordering_attr, LOOM_ATOMIC_ORDERING_COUNT_);
    }
  } else {
    int64_t ordering_attr = 0;
    bool has_ordering = false;
    IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
        state, packet, IREE_SV("ordering"), &has_ordering, &ordering_attr));
    if (!has_ordering) {
      return iree_ok_status();
    }
    if (!loom_llvmir_emit_atomic_ordering(ordering_attr, &success_ordering)) {
      return loom_llvmir_emit_shape_diagnostic(
          state, packet->op, IREE_SV("atomic_ordering"),
          (uint32_t)ordering_attr, LOOM_ATOMIC_ORDERING_COUNT_);
    }
  }

  int64_t scope_attr = 0;
  bool has_scope = false;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_i64_immediate(
      state, packet, IREE_SV("scope"), &has_scope, &scope_attr));
  if (!has_scope) {
    return iree_ok_status();
  }
  iree_string_view_t sync_scope = iree_string_view_empty();
  if (!loom_llvmir_emit_atomic_sync_scope(scope_attr, &sync_scope)) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("atomic_scope"), (uint32_t)scope_attr,
        LOOM_ATOMIC_SCOPE_COUNT_);
  }

  const uint32_t pointer_operand_index = is_cmpxchg ? 2u : 1u;
  const loom_value_id_t base_value_id =
      loom_op_const_operands(packet->op)[pointer_operand_index];
  const loom_llvmir_value_id_t base =
      loom_llvmir_emit_lookup_value(state, base_value_id);
  const loom_llvmir_emit_memory_info_t memory_info = {
      .value_type = info->value_type,
      .unit_count = 1,
      .flags = indexed ? LOOM_LLVMIR_EMIT_MEMORY_FLAG_INDEXED : 0,
  };
  loom_llvmir_value_id_t pointer = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_byte_pointer(
      state, packet, &memory_info, pointer_operand_index, base, &pointer));
  if (pointer == LOOM_LLVMIR_VALUE_ID_INVALID) {
    return iree_ok_status();
  }

  loom_llvmir_type_id_t value_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_core_type(
      state->llvmir_module, info->value_type, /*unit_count=*/1,
      /*pointer_address_space=*/0, &value_type));

  uint32_t alignment = 0;
  if (!loom_llvmir_emit_core_type_byte_count(info->value_type, &alignment)) {
    return loom_llvmir_emit_shape_diagnostic(
        state, packet->op, IREE_SV("atomic_value_type"), 0, 1);
  }

  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  iree_string_view_t result_name = iree_string_view_empty();
  if (returns_value) {
    result_value = loom_op_const_results(packet->op)[0];
    result_name = loom_llvmir_emit_value_name(state->module, result_value);
  }

  if (is_cmpxchg) {
    const loom_llvmir_value_id_t expected = loom_llvmir_emit_lookup_value(
        state, loom_op_const_operands(packet->op)[0]);
    const loom_llvmir_value_id_t replacement = loom_llvmir_emit_lookup_value(
        state, loom_op_const_operands(packet->op)[1]);
    loom_llvmir_type_id_t i1_type = LOOM_LLVMIR_TYPE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_llvmir_module_get_integer_type(state->llvmir_module, 1, &i1_type));
    const loom_llvmir_type_id_t aggregate_elements[] = {value_type, i1_type};
    loom_llvmir_type_id_t aggregate_type = LOOM_LLVMIR_TYPE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_llvmir_module_get_struct_type(
        state->llvmir_module, aggregate_elements,
        IREE_ARRAYSIZE(aggregate_elements), &aggregate_type));
    loom_llvmir_value_id_t aggregate = LOOM_LLVMIR_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_llvmir_build_cmpxchg(state->llvmir_block,
                                  &(loom_llvmir_cmpxchg_desc_t){
                                      .result_type = aggregate_type,
                                      .value_type = value_type,
                                      .pointer = pointer,
                                      .expected = expected,
                                      .replacement = replacement,
                                      .success_ordering = success_ordering,
                                      .failure_ordering = failure_ordering,
                                      .sync_scope = sync_scope,
                                      .alignment = alignment,
                                  },
                                  &aggregate));
    loom_llvmir_value_id_t old_value = LOOM_LLVMIR_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_llvmir_build_extract_value(state->llvmir_block,
                                        &(loom_llvmir_extract_value_desc_t){
                                            .result_name = result_name,
                                            .result_type = value_type,
                                            .aggregate = aggregate,
                                            .index = 0,
                                        },
                                        &old_value));
    loom_llvmir_emit_define_value(state, result_value, old_value);
    return iree_ok_status();
  }

  const loom_llvmir_value_id_t value = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_llvmir_build_atomic_rmw(state->llvmir_block,
                                   &(loom_llvmir_atomic_rmw_desc_t){
                                       .result_name = result_name,
                                       .result_type = value_type,
                                       .op = info->op,
                                       .pointer = pointer,
                                       .value = value,
                                       .ordering = success_ordering,
                                       .sync_scope = sync_scope,
                                       .alignment = alignment,
                                   },
                                   &llvmir_result));
  if (!returns_value) {
    return iree_ok_status();
  }
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_memory_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet, bool* out_matched) {
  *out_matched = true;
  const uint32_t descriptor_ref = packet->descriptor_ordinal;

  const loom_llvmir_emit_alloca_info_t* alloca_info =
      loom_llvmir_emit_lookup_alloca(descriptor_ref);
  if (alloca_info) {
    return loom_llvmir_emit_alloca(state, packet, alloca_info);
  }

  const loom_llvmir_emit_memory_info_t* memory_info =
      loom_llvmir_emit_lookup_memory(descriptor_ref);
  if (memory_info) {
    return loom_llvmir_emit_memory(state, packet, memory_info);
  }

  const loom_llvmir_emit_atomic_info_t* atomic_info =
      loom_llvmir_emit_lookup_atomic(descriptor_ref);
  if (atomic_info) {
    return loom_llvmir_emit_atomic(state, packet, atomic_info);
  }

  *out_matched = false;
  return iree_ok_status();
}
