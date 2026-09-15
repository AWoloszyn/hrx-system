// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/llvmir/function_arithmetic.h"

#include "loom/target/arch/llvmir/descriptors/descriptors.h"

typedef struct loom_llvmir_emit_binary_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // LLVMIR builder binary opcode.
  loom_llvmir_binop_t binop;
} loom_llvmir_emit_binary_info_t;

typedef struct loom_llvmir_emit_binary_intrinsic_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // LLVM intrinsic symbol name.
  iree_string_view_t intrinsic_name;
} loom_llvmir_emit_binary_intrinsic_info_t;

typedef struct loom_llvmir_emit_unary_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // LLVMIR builder unary opcode.
  loom_llvmir_unop_t unop;
} loom_llvmir_emit_unary_info_t;

typedef struct loom_llvmir_emit_unary_intrinsic_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // LLVM intrinsic symbol name.
  iree_string_view_t intrinsic_name;
} loom_llvmir_emit_unary_intrinsic_info_t;

typedef struct loom_llvmir_emit_ternary_intrinsic_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // LLVM intrinsic symbol name.
  iree_string_view_t intrinsic_name;
} loom_llvmir_emit_ternary_intrinsic_info_t;

typedef struct loom_llvmir_emit_compare_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // True when |predicate| stores an LLVM floating-point comparison predicate.
  bool is_float;
  // LLVM comparison predicate stored as loom_llvmir_icmp_predicate_t or
  // loom_llvmir_fcmp_predicate_t according to |is_float|.
  uint8_t predicate;
} loom_llvmir_emit_compare_info_t;

typedef struct loom_llvmir_emit_cast_info_t {
  // Generated descriptor reference ordinal.
  uint32_t descriptor_ref;
  // LLVMIR builder cast opcode.
  loom_llvmir_cast_op_t cast_op;
} loom_llvmir_emit_cast_info_t;

#define LOOM_LLVMIR_BINARY_INFO(suffix, op) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##suffix, LOOM_LLVMIR_BINOP_##op}

#define LOOM_LLVMIR_MASK_BINARY_INFOS(suffix)   \
  LOOM_LLVMIR_BINARY_INFO(AND_##suffix, AND),   \
      LOOM_LLVMIR_BINARY_INFO(OR_##suffix, OR), \
      LOOM_LLVMIR_BINARY_INFO(XOR_##suffix, XOR)

#define LOOM_LLVMIR_SHIFT_BINARY_INFOS(suffix)      \
  LOOM_LLVMIR_BINARY_INFO(SHL_##suffix, SHL),       \
      LOOM_LLVMIR_BINARY_INFO(LSHR_##suffix, LSHR), \
      LOOM_LLVMIR_BINARY_INFO(ASHR_##suffix, ASHR)

#define LOOM_LLVMIR_BITWISE_BINARY_INFOS(suffix) \
  LOOM_LLVMIR_MASK_BINARY_INFOS(suffix), LOOM_LLVMIR_SHIFT_BINARY_INFOS(suffix)

#define LOOM_LLVMIR_INTEGER_BASE_BINARY_INFOS(suffix) \
  LOOM_LLVMIR_BINARY_INFO(ADD_##suffix, ADD),         \
      LOOM_LLVMIR_BINARY_INFO(SUB_##suffix, SUB),     \
      LOOM_LLVMIR_BINARY_INFO(MUL_##suffix, MUL),     \
      LOOM_LLVMIR_BITWISE_BINARY_INFOS(suffix)

#define LOOM_LLVMIR_INTEGER_BINARY_INFOS(suffix)    \
  LOOM_LLVMIR_INTEGER_BASE_BINARY_INFOS(suffix),    \
      LOOM_LLVMIR_BINARY_INFO(UDIV_##suffix, UDIV), \
      LOOM_LLVMIR_BINARY_INFO(SDIV_##suffix, SDIV), \
      LOOM_LLVMIR_BINARY_INFO(UREM_##suffix, UREM), \
      LOOM_LLVMIR_BINARY_INFO(SREM_##suffix, SREM)

#define LOOM_LLVMIR_FLOAT_BINARY_INFOS(suffix)     \
  LOOM_LLVMIR_BINARY_INFO(ADD_##suffix, FADD),     \
      LOOM_LLVMIR_BINARY_INFO(SUB_##suffix, FSUB), \
      LOOM_LLVMIR_BINARY_INFO(MUL_##suffix, FMUL), \
      LOOM_LLVMIR_BINARY_INFO(DIV_##suffix, FDIV)

#define LOOM_LLVMIR_VECTOR_BINARY_INFOS(lanes)              \
  LOOM_LLVMIR_MASK_BINARY_INFOS(V##lanes##I1),              \
      LOOM_LLVMIR_BITWISE_BINARY_INFOS(V##lanes##I8),       \
      LOOM_LLVMIR_BITWISE_BINARY_INFOS(V##lanes##I16),      \
      LOOM_LLVMIR_INTEGER_BASE_BINARY_INFOS(V##lanes##I32), \
      LOOM_LLVMIR_BINARY_INFO(ADD_V##lanes##F32, FADD),     \
      LOOM_LLVMIR_BINARY_INFO(SUB_V##lanes##F32, FSUB),     \
      LOOM_LLVMIR_BINARY_INFO(MUL_V##lanes##F32, FMUL),     \
      LOOM_LLVMIR_BINARY_INFO(DIV_V##lanes##F32, FDIV)

static const loom_llvmir_emit_binary_info_t kBinaryInfos[] = {
    LOOM_LLVMIR_MASK_BINARY_INFOS(I1),
    LOOM_LLVMIR_BITWISE_BINARY_INFOS(I8),
    LOOM_LLVMIR_BITWISE_BINARY_INFOS(I16),
    LOOM_LLVMIR_INTEGER_BINARY_INFOS(I32),
    LOOM_LLVMIR_INTEGER_BINARY_INFOS(I64),
    LOOM_LLVMIR_FLOAT_BINARY_INFOS(F32),
    LOOM_LLVMIR_FLOAT_BINARY_INFOS(F64),
    LOOM_LLVMIR_VECTOR_BINARY_INFOS(2),
    LOOM_LLVMIR_VECTOR_BINARY_INFOS(3),
    LOOM_LLVMIR_VECTOR_BINARY_INFOS(4),
    LOOM_LLVMIR_VECTOR_BINARY_INFOS(8),
    LOOM_LLVMIR_VECTOR_BINARY_INFOS(16),
};

#undef LOOM_LLVMIR_VECTOR_BINARY_INFOS
#undef LOOM_LLVMIR_FLOAT_BINARY_INFOS
#undef LOOM_LLVMIR_INTEGER_BINARY_INFOS
#undef LOOM_LLVMIR_INTEGER_BASE_BINARY_INFOS
#undef LOOM_LLVMIR_BITWISE_BINARY_INFOS
#undef LOOM_LLVMIR_SHIFT_BINARY_INFOS
#undef LOOM_LLVMIR_MASK_BINARY_INFOS
#undef LOOM_LLVMIR_BINARY_INFO

#define LOOM_LLVMIR_BINARY_INTRINSIC_INFO(suffix, name) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##suffix, IREE_SVL(name)}

#define LOOM_LLVMIR_MINNUM_INFO(suffix, name) \
  LOOM_LLVMIR_BINARY_INTRINSIC_INFO(MINNUM_##suffix, "llvm.minnum." name)

#define LOOM_LLVMIR_MAXNUM_INFO(suffix, name) \
  LOOM_LLVMIR_BINARY_INTRINSIC_INFO(MAXNUM_##suffix, "llvm.maxnum." name)

static const loom_llvmir_emit_binary_intrinsic_info_t kBinaryIntrinsicInfos[] =
    {
        LOOM_LLVMIR_MINNUM_INFO(F32, "f32"),
        LOOM_LLVMIR_MINNUM_INFO(F64, "f64"),
        LOOM_LLVMIR_MINNUM_INFO(V2F32, "v2f32"),
        LOOM_LLVMIR_MINNUM_INFO(V3F32, "v3f32"),
        LOOM_LLVMIR_MINNUM_INFO(V4F32, "v4f32"),
        LOOM_LLVMIR_MINNUM_INFO(V8F32, "v8f32"),
        LOOM_LLVMIR_MINNUM_INFO(V16F32, "v16f32"),
        LOOM_LLVMIR_MAXNUM_INFO(F32, "f32"),
        LOOM_LLVMIR_MAXNUM_INFO(F64, "f64"),
        LOOM_LLVMIR_MAXNUM_INFO(V2F32, "v2f32"),
        LOOM_LLVMIR_MAXNUM_INFO(V3F32, "v3f32"),
        LOOM_LLVMIR_MAXNUM_INFO(V4F32, "v4f32"),
        LOOM_LLVMIR_MAXNUM_INFO(V8F32, "v8f32"),
        LOOM_LLVMIR_MAXNUM_INFO(V16F32, "v16f32"),
};

#undef LOOM_LLVMIR_MAXNUM_INFO
#undef LOOM_LLVMIR_MINNUM_INFO
#undef LOOM_LLVMIR_BINARY_INTRINSIC_INFO

#define LOOM_LLVMIR_UNARY_INFO(suffix, op) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##suffix, LOOM_LLVMIR_UNOP_##op}

#define LOOM_LLVMIR_NEG_INFO(suffix) LOOM_LLVMIR_UNARY_INFO(NEG_##suffix, FNEG)

static const loom_llvmir_emit_unary_info_t kUnaryInfos[] = {
    LOOM_LLVMIR_NEG_INFO(F32),    LOOM_LLVMIR_NEG_INFO(F64),
    LOOM_LLVMIR_NEG_INFO(V2F32),  LOOM_LLVMIR_NEG_INFO(V3F32),
    LOOM_LLVMIR_NEG_INFO(V4F32),  LOOM_LLVMIR_NEG_INFO(V8F32),
    LOOM_LLVMIR_NEG_INFO(V16F32),
};

#undef LOOM_LLVMIR_NEG_INFO
#undef LOOM_LLVMIR_UNARY_INFO

#define LOOM_LLVMIR_UNARY_INTRINSIC_INFO(suffix, name) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##suffix, IREE_SVL(name)}

#define LOOM_LLVMIR_FABS_INFO(suffix, name) \
  LOOM_LLVMIR_UNARY_INTRINSIC_INFO(ABS_##suffix, "llvm.fabs." name)

static const loom_llvmir_emit_unary_intrinsic_info_t kUnaryIntrinsicInfos[] = {
    LOOM_LLVMIR_FABS_INFO(F32, "f32"),
    LOOM_LLVMIR_FABS_INFO(F64, "f64"),
    LOOM_LLVMIR_FABS_INFO(V2F32, "v2f32"),
    LOOM_LLVMIR_FABS_INFO(V3F32, "v3f32"),
    LOOM_LLVMIR_FABS_INFO(V4F32, "v4f32"),
    LOOM_LLVMIR_FABS_INFO(V8F32, "v8f32"),
    LOOM_LLVMIR_FABS_INFO(V16F32, "v16f32"),
};

#undef LOOM_LLVMIR_FABS_INFO
#undef LOOM_LLVMIR_UNARY_INTRINSIC_INFO

#define LOOM_LLVMIR_TERNARY_INTRINSIC_INFO(suffix, name) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##suffix, IREE_SVL(name)}

#define LOOM_LLVMIR_FMA_INFO(suffix, name) \
  LOOM_LLVMIR_TERNARY_INTRINSIC_INFO(FMA_##suffix, "llvm.fma." name)

static const loom_llvmir_emit_ternary_intrinsic_info_t
    kTernaryIntrinsicInfos[] = {
        LOOM_LLVMIR_FMA_INFO(F32, "f32"),
        LOOM_LLVMIR_FMA_INFO(F64, "f64"),
        LOOM_LLVMIR_FMA_INFO(V2F32, "v2f32"),
        LOOM_LLVMIR_FMA_INFO(V3F32, "v3f32"),
        LOOM_LLVMIR_FMA_INFO(V4F32, "v4f32"),
        LOOM_LLVMIR_FMA_INFO(V8F32, "v8f32"),
        LOOM_LLVMIR_FMA_INFO(V16F32, "v16f32"),
};

#undef LOOM_LLVMIR_FMA_INFO
#undef LOOM_LLVMIR_TERNARY_INTRINSIC_INFO

#define LOOM_LLVMIR_ICMP_INFO(suffix, predicate)                         \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_CMP_##predicate##_##suffix, false, \
   LOOM_LLVMIR_ICMP_##predicate}

#define LOOM_LLVMIR_FCMP_INFO(suffix, predicate)                        \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_CMP_##predicate##_##suffix, true, \
   LOOM_LLVMIR_FCMP_##predicate}

#define LOOM_LLVMIR_ICMP_INFOS(suffix)                                        \
  LOOM_LLVMIR_ICMP_INFO(suffix, EQ), LOOM_LLVMIR_ICMP_INFO(suffix, NE),       \
      LOOM_LLVMIR_ICMP_INFO(suffix, SLT), LOOM_LLVMIR_ICMP_INFO(suffix, SLE), \
      LOOM_LLVMIR_ICMP_INFO(suffix, SGT), LOOM_LLVMIR_ICMP_INFO(suffix, SGE), \
      LOOM_LLVMIR_ICMP_INFO(suffix, ULT), LOOM_LLVMIR_ICMP_INFO(suffix, ULE), \
      LOOM_LLVMIR_ICMP_INFO(suffix, UGT), LOOM_LLVMIR_ICMP_INFO(suffix, UGE)

#define LOOM_LLVMIR_FCMP_INFOS(suffix)                                        \
  LOOM_LLVMIR_FCMP_INFO(suffix, OEQ), LOOM_LLVMIR_FCMP_INFO(suffix, OGT),     \
      LOOM_LLVMIR_FCMP_INFO(suffix, OGE), LOOM_LLVMIR_FCMP_INFO(suffix, OLT), \
      LOOM_LLVMIR_FCMP_INFO(suffix, OLE), LOOM_LLVMIR_FCMP_INFO(suffix, ONE), \
      LOOM_LLVMIR_FCMP_INFO(suffix, ORD), LOOM_LLVMIR_FCMP_INFO(suffix, UEQ), \
      LOOM_LLVMIR_FCMP_INFO(suffix, UGT), LOOM_LLVMIR_FCMP_INFO(suffix, UGE), \
      LOOM_LLVMIR_FCMP_INFO(suffix, ULT), LOOM_LLVMIR_FCMP_INFO(suffix, ULE), \
      LOOM_LLVMIR_FCMP_INFO(suffix, UNE), LOOM_LLVMIR_FCMP_INFO(suffix, UNO)

#define LOOM_LLVMIR_VECTOR_COMPARE_INFOS(lanes) \
  LOOM_LLVMIR_ICMP_INFOS(V##lanes##I32), LOOM_LLVMIR_FCMP_INFOS(V##lanes##F32)

static const loom_llvmir_emit_compare_info_t kCompareInfos[] = {
    LOOM_LLVMIR_ICMP_INFOS(I32),          LOOM_LLVMIR_ICMP_INFOS(I64),
    LOOM_LLVMIR_FCMP_INFOS(F32),          LOOM_LLVMIR_FCMP_INFOS(F64),
    LOOM_LLVMIR_VECTOR_COMPARE_INFOS(2),  LOOM_LLVMIR_VECTOR_COMPARE_INFOS(3),
    LOOM_LLVMIR_VECTOR_COMPARE_INFOS(4),  LOOM_LLVMIR_VECTOR_COMPARE_INFOS(8),
    LOOM_LLVMIR_VECTOR_COMPARE_INFOS(16),
};

#undef LOOM_LLVMIR_VECTOR_COMPARE_INFOS
#undef LOOM_LLVMIR_FCMP_INFOS
#undef LOOM_LLVMIR_ICMP_INFOS
#undef LOOM_LLVMIR_FCMP_INFO
#undef LOOM_LLVMIR_ICMP_INFO

#define LOOM_LLVMIR_CAST_INFO(stem, source, result, op) \
  {LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_##stem##_##source##_##result, op}

#define LOOM_LLVMIR_SCALAR_CAST_INFOS()                                        \
  LOOM_LLVMIR_CAST_INFO(TRUNC, I32, I8, LOOM_LLVMIR_CAST_TRUNCATE),            \
      LOOM_LLVMIR_CAST_INFO(TRUNC, I32, I16, LOOM_LLVMIR_CAST_TRUNCATE),       \
      LOOM_LLVMIR_CAST_INFO(TRUNC, I64, I32, LOOM_LLVMIR_CAST_TRUNCATE),       \
      LOOM_LLVMIR_CAST_INFO(SEXT, I8, I32, LOOM_LLVMIR_CAST_SIGN_EXTEND),      \
      LOOM_LLVMIR_CAST_INFO(SEXT, I16, I32, LOOM_LLVMIR_CAST_SIGN_EXTEND),     \
      LOOM_LLVMIR_CAST_INFO(SEXT, I32, I64, LOOM_LLVMIR_CAST_SIGN_EXTEND),     \
      LOOM_LLVMIR_CAST_INFO(ZEXT, I8, I32, LOOM_LLVMIR_CAST_ZERO_EXTEND),      \
      LOOM_LLVMIR_CAST_INFO(ZEXT, I16, I32, LOOM_LLVMIR_CAST_ZERO_EXTEND),     \
      LOOM_LLVMIR_CAST_INFO(ZEXT, I32, I64, LOOM_LLVMIR_CAST_ZERO_EXTEND),     \
      LOOM_LLVMIR_CAST_INFO(SITOFP, I8, F32,                                   \
                            LOOM_LLVMIR_CAST_SIGNED_INT_TO_FP),                \
      LOOM_LLVMIR_CAST_INFO(SITOFP, I32, F32,                                  \
                            LOOM_LLVMIR_CAST_SIGNED_INT_TO_FP),                \
      LOOM_LLVMIR_CAST_INFO(SITOFP, I64, F64,                                  \
                            LOOM_LLVMIR_CAST_SIGNED_INT_TO_FP),                \
      LOOM_LLVMIR_CAST_INFO(UITOFP, I8, F32,                                   \
                            LOOM_LLVMIR_CAST_UNSIGNED_INT_TO_FP),              \
      LOOM_LLVMIR_CAST_INFO(UITOFP, I32, F32,                                  \
                            LOOM_LLVMIR_CAST_UNSIGNED_INT_TO_FP),              \
      LOOM_LLVMIR_CAST_INFO(UITOFP, I64, F64,                                  \
                            LOOM_LLVMIR_CAST_UNSIGNED_INT_TO_FP),              \
      LOOM_LLVMIR_CAST_INFO(FPTOSI, F32, I32,                                  \
                            LOOM_LLVMIR_CAST_FP_TO_SIGNED_INT),                \
      LOOM_LLVMIR_CAST_INFO(FPTOSI, F64, I64,                                  \
                            LOOM_LLVMIR_CAST_FP_TO_SIGNED_INT),                \
      LOOM_LLVMIR_CAST_INFO(FPTOUI, F32, I32,                                  \
                            LOOM_LLVMIR_CAST_FP_TO_UNSIGNED_INT),              \
      LOOM_LLVMIR_CAST_INFO(FPTOUI, F64, I64,                                  \
                            LOOM_LLVMIR_CAST_FP_TO_UNSIGNED_INT),              \
      LOOM_LLVMIR_CAST_INFO(FPTRUNC, F32, F16, LOOM_LLVMIR_CAST_FP_TRUNCATE),  \
      LOOM_LLVMIR_CAST_INFO(FPTRUNC, F32, BF16, LOOM_LLVMIR_CAST_FP_TRUNCATE), \
      LOOM_LLVMIR_CAST_INFO(FPTRUNC, F64, F32, LOOM_LLVMIR_CAST_FP_TRUNCATE),  \
      LOOM_LLVMIR_CAST_INFO(FPEXT, F16, F32, LOOM_LLVMIR_CAST_FP_EXTEND),      \
      LOOM_LLVMIR_CAST_INFO(FPEXT, BF16, F32, LOOM_LLVMIR_CAST_FP_EXTEND),     \
      LOOM_LLVMIR_CAST_INFO(FPEXT, F32, F64, LOOM_LLVMIR_CAST_FP_EXTEND),      \
      LOOM_LLVMIR_CAST_INFO(BITCAST, I16, F16, LOOM_LLVMIR_CAST_BITCAST),      \
      LOOM_LLVMIR_CAST_INFO(BITCAST, I16, BF16, LOOM_LLVMIR_CAST_BITCAST),     \
      LOOM_LLVMIR_CAST_INFO(BITCAST, F16, I16, LOOM_LLVMIR_CAST_BITCAST),      \
      LOOM_LLVMIR_CAST_INFO(BITCAST, BF16, I16, LOOM_LLVMIR_CAST_BITCAST),     \
      LOOM_LLVMIR_CAST_INFO(BITCAST, F16, BF16, LOOM_LLVMIR_CAST_BITCAST),     \
      LOOM_LLVMIR_CAST_INFO(BITCAST, BF16, F16, LOOM_LLVMIR_CAST_BITCAST),     \
      LOOM_LLVMIR_CAST_INFO(BITCAST, I32, F32, LOOM_LLVMIR_CAST_BITCAST),      \
      LOOM_LLVMIR_CAST_INFO(BITCAST, F32, I32, LOOM_LLVMIR_CAST_BITCAST),      \
      LOOM_LLVMIR_CAST_INFO(BITCAST, I64, F64, LOOM_LLVMIR_CAST_BITCAST),      \
      LOOM_LLVMIR_CAST_INFO(BITCAST, F64, I64, LOOM_LLVMIR_CAST_BITCAST)

#define LOOM_LLVMIR_VECTOR_CAST_INFOS(lanes)                        \
  LOOM_LLVMIR_CAST_INFO(SEXT, V##lanes##I32, V##lanes##I64,         \
                        LOOM_LLVMIR_CAST_SIGN_EXTEND),              \
      LOOM_LLVMIR_CAST_INFO(ZEXT, V##lanes##I32, V##lanes##I64,     \
                            LOOM_LLVMIR_CAST_ZERO_EXTEND),          \
      LOOM_LLVMIR_CAST_INFO(TRUNC, V##lanes##I64, V##lanes##I32,    \
                            LOOM_LLVMIR_CAST_TRUNCATE),             \
      LOOM_LLVMIR_CAST_INFO(SITOFP, V##lanes##I32, V##lanes##F32,   \
                            LOOM_LLVMIR_CAST_SIGNED_INT_TO_FP),     \
      LOOM_LLVMIR_CAST_INFO(UITOFP, V##lanes##I32, V##lanes##F32,   \
                            LOOM_LLVMIR_CAST_UNSIGNED_INT_TO_FP),   \
      LOOM_LLVMIR_CAST_INFO(FPTOSI, V##lanes##F32, V##lanes##I32,   \
                            LOOM_LLVMIR_CAST_FP_TO_SIGNED_INT),     \
      LOOM_LLVMIR_CAST_INFO(FPTOUI, V##lanes##F32, V##lanes##I32,   \
                            LOOM_LLVMIR_CAST_FP_TO_UNSIGNED_INT),   \
      LOOM_LLVMIR_CAST_INFO(FPTRUNC, V##lanes##F32, V##lanes##F16,  \
                            LOOM_LLVMIR_CAST_FP_TRUNCATE),          \
      LOOM_LLVMIR_CAST_INFO(FPTRUNC, V##lanes##F32, V##lanes##BF16, \
                            LOOM_LLVMIR_CAST_FP_TRUNCATE),          \
      LOOM_LLVMIR_CAST_INFO(FPEXT, V##lanes##F16, V##lanes##F32,    \
                            LOOM_LLVMIR_CAST_FP_EXTEND),            \
      LOOM_LLVMIR_CAST_INFO(FPEXT, V##lanes##BF16, V##lanes##F32,   \
                            LOOM_LLVMIR_CAST_FP_EXTEND),            \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V##lanes##I32, V##lanes##F32,  \
                            LOOM_LLVMIR_CAST_BITCAST),              \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V##lanes##F32, V##lanes##I32,  \
                            LOOM_LLVMIR_CAST_BITCAST),              \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V##lanes##I64, V##lanes##F64,  \
                            LOOM_LLVMIR_CAST_BITCAST),              \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V##lanes##F64, V##lanes##I64,  \
                            LOOM_LLVMIR_CAST_BITCAST)

#define LOOM_LLVMIR_BITCAST_RESHAPE_INFOS()                                  \
  LOOM_LLVMIR_CAST_INFO(BITCAST, I32, V4I8, LOOM_LLVMIR_CAST_BITCAST),       \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V2I32, V8I8, LOOM_LLVMIR_CAST_BITCAST), \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V2F16, I32, LOOM_LLVMIR_CAST_BITCAST),  \
      LOOM_LLVMIR_CAST_INFO(BITCAST, V2BF16, I32, LOOM_LLVMIR_CAST_BITCAST)

static const loom_llvmir_emit_cast_info_t kCastInfos[] = {
    LOOM_LLVMIR_SCALAR_CAST_INFOS(),     LOOM_LLVMIR_VECTOR_CAST_INFOS(2),
    LOOM_LLVMIR_VECTOR_CAST_INFOS(3),    LOOM_LLVMIR_VECTOR_CAST_INFOS(4),
    LOOM_LLVMIR_VECTOR_CAST_INFOS(8),    LOOM_LLVMIR_VECTOR_CAST_INFOS(16),
    LOOM_LLVMIR_BITCAST_RESHAPE_INFOS(),
};

#undef LOOM_LLVMIR_BITCAST_RESHAPE_INFOS
#undef LOOM_LLVMIR_VECTOR_CAST_INFOS
#undef LOOM_LLVMIR_SCALAR_CAST_INFOS
#undef LOOM_LLVMIR_CAST_INFO

#define LOOM_LLVMIR_SELECT_REF(suffix) \
  LLVMIR_GENERIC_CORE_DESCRIPTOR_REF_SELECT_##suffix

#define LOOM_LLVMIR_VECTOR_SELECT_REFS(lanes)                                  \
  LOOM_LLVMIR_SELECT_REF(V##lanes##I8), LOOM_LLVMIR_SELECT_REF(V##lanes##I16), \
      LOOM_LLVMIR_SELECT_REF(V##lanes##I32),                                   \
      LOOM_LLVMIR_SELECT_REF(V##lanes##I64),                                   \
      LOOM_LLVMIR_SELECT_REF(V##lanes##F16),                                   \
      LOOM_LLVMIR_SELECT_REF(V##lanes##BF16),                                  \
      LOOM_LLVMIR_SELECT_REF(V##lanes##F32),                                   \
      LOOM_LLVMIR_SELECT_REF(V##lanes##F64)

static const uint32_t kSelectDescriptorRefs[] = {
    LOOM_LLVMIR_SELECT_REF(I1),        LOOM_LLVMIR_SELECT_REF(I32),
    LOOM_LLVMIR_SELECT_REF(I64),       LOOM_LLVMIR_SELECT_REF(F32),
    LOOM_LLVMIR_SELECT_REF(F64),       LOOM_LLVMIR_VECTOR_SELECT_REFS(2),
    LOOM_LLVMIR_VECTOR_SELECT_REFS(3), LOOM_LLVMIR_VECTOR_SELECT_REFS(4),
    LOOM_LLVMIR_VECTOR_SELECT_REFS(8), LOOM_LLVMIR_VECTOR_SELECT_REFS(16),
};

#undef LOOM_LLVMIR_VECTOR_SELECT_REFS
#undef LOOM_LLVMIR_SELECT_REF

static const loom_llvmir_emit_binary_info_t* loom_llvmir_emit_lookup_binary(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kBinaryInfos); ++i) {
    if (kBinaryInfos[i].descriptor_ref == descriptor_ref) {
      return &kBinaryInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_binary_intrinsic_info_t*
loom_llvmir_emit_lookup_binary_intrinsic(uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kBinaryIntrinsicInfos); ++i) {
    if (kBinaryIntrinsicInfos[i].descriptor_ref == descriptor_ref) {
      return &kBinaryIntrinsicInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_unary_info_t* loom_llvmir_emit_lookup_unary(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kUnaryInfos); ++i) {
    if (kUnaryInfos[i].descriptor_ref == descriptor_ref) {
      return &kUnaryInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_unary_intrinsic_info_t*
loom_llvmir_emit_lookup_unary_intrinsic(uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kUnaryIntrinsicInfos); ++i) {
    if (kUnaryIntrinsicInfos[i].descriptor_ref == descriptor_ref) {
      return &kUnaryIntrinsicInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_ternary_intrinsic_info_t*
loom_llvmir_emit_lookup_ternary_intrinsic(uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kTernaryIntrinsicInfos);
       ++i) {
    if (kTernaryIntrinsicInfos[i].descriptor_ref == descriptor_ref) {
      return &kTernaryIntrinsicInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_compare_info_t* loom_llvmir_emit_lookup_compare(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kCompareInfos); ++i) {
    if (kCompareInfos[i].descriptor_ref == descriptor_ref) {
      return &kCompareInfos[i];
    }
  }
  return NULL;
}

static const loom_llvmir_emit_cast_info_t* loom_llvmir_emit_lookup_cast(
    uint32_t descriptor_ref) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kCastInfos); ++i) {
    if (kCastInfos[i].descriptor_ref == descriptor_ref) {
      return &kCastInfos[i];
    }
  }
  return NULL;
}

static bool loom_llvmir_emit_is_select(uint32_t descriptor_ref) {
  return loom_llvmir_emit_descriptor_ref_in(
      descriptor_ref, kSelectDescriptorRefs,
      IREE_ARRAYSIZE(kSelectDescriptorRefs));
}

static iree_status_t loom_llvmir_emit_binary(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_binary_info_t* info) {
  if (packet->op->operand_count != 2) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 2);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_llvmir_value_id_t lhs = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  const loom_llvmir_value_id_t rhs = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[1]);
  int64_t fast_math_flags = 0;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_read_optional_i64_immediate(
      state, packet, IREE_SV("fast_math_flags"), &fast_math_flags));
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_binop(
      state->llvmir_block,
      &(loom_llvmir_binop_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .op = info->binop,
          .lhs = lhs,
          .rhs = rhs,
          .fast_math_flags = (loom_llvmir_fast_math_flags_t)fast_math_flags,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_unary(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_unary_info_t* info) {
  if (packet->op->operand_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 1);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_llvmir_value_id_t input = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_unop(
      state->llvmir_block,
      &(loom_llvmir_unop_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .op = info->unop,
          .value = input,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_declare_same_type_intrinsic(
    loom_llvmir_emit_function_state_t* state, iree_string_view_t name,
    loom_llvmir_type_id_t type_id, uint32_t parameter_count,
    loom_llvmir_function_t** out_function) {
  *out_function = loom_llvmir_module_find_function(state->llvmir_module, name);
  if (*out_function != NULL) return iree_ok_status();

  loom_llvmir_function_desc_t desc = {
      .kind = LOOM_LLVMIR_FUNCTION_DECLARATION,
      .name = name,
      .return_type = type_id,
      .linkage = LOOM_LLVMIR_LINKAGE_DEFAULT,
      .calling_convention = LOOM_LLVMIR_CALLING_CONVENTION_DEFAULT,
      .attr_group_id = LOOM_LLVMIR_ATTR_GROUP_ID_INVALID,
  };
  IREE_RETURN_IF_ERROR(loom_llvmir_module_add_function(state->llvmir_module,
                                                       &desc, out_function));
  loom_llvmir_value_id_t ignored = LOOM_LLVMIR_VALUE_ID_INVALID;
  for (uint32_t i = 0; i < parameter_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_llvmir_function_add_parameter(
        *out_function, &(loom_llvmir_parameter_desc_t){.type_id = type_id},
        &ignored));
  }
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_binary_intrinsic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_binary_intrinsic_info_t* info) {
  if (packet->op->operand_count != 2) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 2);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  loom_llvmir_value_id_t args[2] = {
      LOOM_LLVMIR_VALUE_ID_INVALID,
      LOOM_LLVMIR_VALUE_ID_INVALID,
  };
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(args); ++i) {
    args[i] = loom_llvmir_emit_lookup_value(state, operands[i]);
  }

  loom_llvmir_function_t* intrinsic = NULL;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_declare_same_type_intrinsic(
      state, info->intrinsic_name, result_type, IREE_ARRAYSIZE(args),
      &intrinsic));
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_call(
      state->llvmir_block,
      &(loom_llvmir_call_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .callee = loom_llvmir_function_id(intrinsic),
          .args = args,
          .arg_count = IREE_ARRAYSIZE(args),
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_unary_intrinsic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_unary_intrinsic_info_t* info) {
  if (packet->op->operand_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 1);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_llvmir_value_id_t arg = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  loom_llvmir_function_t* intrinsic = NULL;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_declare_same_type_intrinsic(
      state, info->intrinsic_name, result_type, 1, &intrinsic));
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_call(
      state->llvmir_block,
      &(loom_llvmir_call_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .callee = loom_llvmir_function_id(intrinsic),
          .args = &arg,
          .arg_count = 1,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_ternary_intrinsic(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_ternary_intrinsic_info_t* info) {
  if (packet->op->operand_count != 3) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 3);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  loom_llvmir_value_id_t args[3] = {
      LOOM_LLVMIR_VALUE_ID_INVALID,
      LOOM_LLVMIR_VALUE_ID_INVALID,
      LOOM_LLVMIR_VALUE_ID_INVALID,
  };
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(args); ++i) {
    args[i] = loom_llvmir_emit_lookup_value(state, operands[i]);
  }

  loom_llvmir_function_t* intrinsic = NULL;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_declare_same_type_intrinsic(
      state, info->intrinsic_name, result_type, IREE_ARRAYSIZE(args),
      &intrinsic));
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_call(
      state->llvmir_block,
      &(loom_llvmir_call_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .callee = loom_llvmir_function_id(intrinsic),
          .args = args,
          .arg_count = IREE_ARRAYSIZE(args),
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_compare(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_compare_info_t* info) {
  if (packet->op->operand_count != 2) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 2);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_llvmir_value_id_t lhs = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  const loom_llvmir_value_id_t rhs = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[1]);

  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  if (info->is_float) {
    IREE_RETURN_IF_ERROR(loom_llvmir_build_fcmp(
        state->llvmir_block,
        &(loom_llvmir_fcmp_desc_t){
            .result_name =
                loom_llvmir_emit_value_name(state->module, result_value),
            .result_type = result_type,
            .predicate = (loom_llvmir_fcmp_predicate_t)info->predicate,
            .lhs = lhs,
            .rhs = rhs,
        },
        &llvmir_result));
  } else {
    IREE_RETURN_IF_ERROR(loom_llvmir_build_icmp(
        state->llvmir_block,
        &(loom_llvmir_icmp_desc_t){
            .result_name =
                loom_llvmir_emit_value_name(state->module, result_value),
            .result_type = result_type,
            .predicate = (loom_llvmir_icmp_predicate_t)info->predicate,
            .lhs = lhs,
            .rhs = rhs,
        },
        &llvmir_result));
  }
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_cast(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet,
    const loom_llvmir_emit_cast_info_t* info) {
  if (packet->op->operand_count != 1) {
    return loom_llvmir_emit_shape_diagnostic(state, packet->op,
                                             IREE_SV("packet_operand"),
                                             packet->op->operand_count, 1);
  }
  loom_llvmir_type_id_t result_type = LOOM_LLVMIR_TYPE_ID_INVALID;
  loom_value_id_t result_value = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_emit_prepare_packet_result(
      state, packet, &result_type, &result_value));
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_llvmir_value_id_t value = loom_llvmir_emit_lookup_value(
      state, loom_op_const_operands(packet->op)[0]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_cast(
      state->llvmir_block,
      &(loom_llvmir_cast_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .op = info->cast_op,
          .value = value,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

static iree_status_t loom_llvmir_emit_select(
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
  if (result_type == LOOM_LLVMIR_TYPE_ID_INVALID) return iree_ok_status();

  const loom_value_id_t* operands = loom_op_const_operands(packet->op);
  const loom_llvmir_value_id_t condition =
      loom_llvmir_emit_lookup_value(state, operands[0]);
  const loom_llvmir_value_id_t true_value =
      loom_llvmir_emit_lookup_value(state, operands[1]);
  const loom_llvmir_value_id_t false_value =
      loom_llvmir_emit_lookup_value(state, operands[2]);
  loom_llvmir_value_id_t llvmir_result = LOOM_LLVMIR_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_llvmir_build_select(
      state->llvmir_block,
      &(loom_llvmir_select_desc_t){
          .result_name =
              loom_llvmir_emit_value_name(state->module, result_value),
          .result_type = result_type,
          .condition = condition,
          .true_value = true_value,
          .false_value = false_value,
      },
      &llvmir_result));
  loom_llvmir_emit_define_value(state, result_value, llvmir_result);
  return iree_ok_status();
}

iree_status_t loom_llvmir_emit_arithmetic_packet(
    loom_llvmir_emit_function_state_t* state,
    const loom_low_descriptor_packet_t* packet, bool* out_matched) {
  *out_matched = true;
  const uint32_t descriptor_ref = packet->descriptor_ordinal;

  const loom_llvmir_emit_binary_info_t* binary_info =
      loom_llvmir_emit_lookup_binary(descriptor_ref);
  if (binary_info) {
    return loom_llvmir_emit_binary(state, packet, binary_info);
  }

  const loom_llvmir_emit_binary_intrinsic_info_t* binary_intrinsic_info =
      loom_llvmir_emit_lookup_binary_intrinsic(descriptor_ref);
  if (binary_intrinsic_info) {
    return loom_llvmir_emit_binary_intrinsic(state, packet,
                                             binary_intrinsic_info);
  }

  const loom_llvmir_emit_unary_info_t* unary_info =
      loom_llvmir_emit_lookup_unary(descriptor_ref);
  if (unary_info) {
    return loom_llvmir_emit_unary(state, packet, unary_info);
  }

  const loom_llvmir_emit_unary_intrinsic_info_t* unary_intrinsic_info =
      loom_llvmir_emit_lookup_unary_intrinsic(descriptor_ref);
  if (unary_intrinsic_info) {
    return loom_llvmir_emit_unary_intrinsic(state, packet,
                                            unary_intrinsic_info);
  }

  const loom_llvmir_emit_ternary_intrinsic_info_t* ternary_intrinsic_info =
      loom_llvmir_emit_lookup_ternary_intrinsic(descriptor_ref);
  if (ternary_intrinsic_info) {
    return loom_llvmir_emit_ternary_intrinsic(state, packet,
                                              ternary_intrinsic_info);
  }

  const loom_llvmir_emit_compare_info_t* compare_info =
      loom_llvmir_emit_lookup_compare(descriptor_ref);
  if (compare_info) {
    return loom_llvmir_emit_compare(state, packet, compare_info);
  }

  const loom_llvmir_emit_cast_info_t* cast_info =
      loom_llvmir_emit_lookup_cast(descriptor_ref);
  if (cast_info) {
    return loom_llvmir_emit_cast(state, packet, cast_info);
  }

  if (loom_llvmir_emit_is_select(descriptor_ref)) {
    return loom_llvmir_emit_select(state, packet);
  }

  *out_matched = false;
  return iree_ok_status();
}
