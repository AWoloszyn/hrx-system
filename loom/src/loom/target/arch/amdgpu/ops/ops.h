// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// GENERATED FILE: DO NOT EDIT.
// Generator: loom.gen.ops.c_tables.
// Regenerate: python3 loom/py/loom/gen/run.py c_tables --in-place
// clang-format off

#ifndef LOOM_OPS_AMDGPU_OPS_H_
#define LOOM_OPS_AMDGPU_OPS_H_

#include "loom/ops/op_defs.h"
#include "loom/target/types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LOOM_OP_AMDGPU_TARGET = LOOM_OP_KIND(LOOM_DIALECT_AMDGPU, 0),
  LOOM_OP_AMDGPU_COUNT_ = 1,
};

// AMDGPU target row selected by amdgpu.target.
typedef enum loom_amdgpu_target_kind_e {
  LOOM_AMDGPU_TARGET_KIND_GFX942 = 1,
  LOOM_AMDGPU_TARGET_KIND_GFX950 = 2,
  LOOM_AMDGPU_TARGET_KIND_GFX1100 = 3,
  LOOM_AMDGPU_TARGET_KIND_GFX1200 = 4,
  LOOM_AMDGPU_TARGET_KIND_GFX1250 = 5,
  LOOM_AMDGPU_TARGET_KIND_GFX1150 = 6,
  LOOM_AMDGPU_TARGET_KIND_GFX11_GENERIC = 7,
  LOOM_AMDGPU_TARGET_KIND_GFX12_GENERIC = 8,
  LOOM_AMDGPU_TARGET_KIND_GFX12_5_GENERIC = 9,
  LOOM_AMDGPU_TARGET_KIND_GFX940 = 10,
  LOOM_AMDGPU_TARGET_KIND_GFX941 = 11,
  LOOM_AMDGPU_TARGET_KIND_GFX1101 = 12,
  LOOM_AMDGPU_TARGET_KIND_GFX1102 = 13,
  LOOM_AMDGPU_TARGET_KIND_GFX1103 = 14,
  LOOM_AMDGPU_TARGET_KIND_GFX1151 = 15,
  LOOM_AMDGPU_TARGET_KIND_GFX1152 = 16,
  LOOM_AMDGPU_TARGET_KIND_GFX1153 = 17,
  LOOM_AMDGPU_TARGET_KIND_GFX1170 = 18,
  LOOM_AMDGPU_TARGET_KIND_GFX1171 = 19,
  LOOM_AMDGPU_TARGET_KIND_GFX1172 = 20,
  LOOM_AMDGPU_TARGET_KIND_GFX1201 = 21,
  LOOM_AMDGPU_TARGET_KIND_GFX1251 = 22,
  LOOM_AMDGPU_TARGET_KIND_GFX9_4_GENERIC = 23,
  LOOM_AMDGPU_TARGET_KIND_GFX1250_A0 = 24,
  LOOM_AMDGPU_TARGET_KIND_COUNT_ = 25,
} loom_amdgpu_target_kind_t;

// Configurable AMDHSA target-ID feature.
typedef enum loom_amdgpu_target_features_e {
  LOOM_AMDGPU_TARGET_FEATURES_SRAMECC = 0,
  LOOM_AMDGPU_TARGET_FEATURES_XNACK = 1,
  LOOM_AMDGPU_TARGET_FEATURES_COUNT_ = 2,
} loom_amdgpu_target_features_t;

// LOOM_OP_AMDGPU_TARGET: AMDGPU target record. The selector chooses one exact, generic, or overlay target row; optional attrs preserve authored common facts and target-ID feature assertions.
// amdgpu.target<gfx11-generic> @gfx11_generic
LOOM_DEFINE_ISA(loom_amdgpu_target_isa, LOOM_OP_AMDGPU_TARGET)
enum {
  LOOM_AMDGPU_TARGET_SYMBOL_ATTR_INDEX = 0,
  LOOM_AMDGPU_TARGET_KIND_ATTR_INDEX = 1,
  LOOM_AMDGPU_TARGET_CODEGEN_FORMAT_ATTR_INDEX = 2,
  LOOM_AMDGPU_TARGET_ARTIFACT_FORMAT_ATTR_INDEX = 3,
  LOOM_AMDGPU_TARGET_DEFAULT_POINTER_BITWIDTH_ATTR_INDEX = 4,
  LOOM_AMDGPU_TARGET_INDEX_BITWIDTH_ATTR_INDEX = 5,
  LOOM_AMDGPU_TARGET_OFFSET_BITWIDTH_ATTR_INDEX = 6,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_X_ATTR_INDEX = 7,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Y_ATTR_INDEX = 8,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Z_ATTR_INDEX = 9,
  LOOM_AMDGPU_TARGET_MAX_FLAT_WORKGROUP_SIZE_ATTR_INDEX = 10,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_STORAGE_BYTES_ATTR_INDEX = 11,
  LOOM_AMDGPU_TARGET_SUBGROUP_SIZE_ATTR_INDEX = 12,
  LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_X_ATTR_INDEX = 13,
  LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Y_ATTR_INDEX = 14,
  LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Z_ATTR_INDEX = 15,
  LOOM_AMDGPU_TARGET_MAX_FLAT_GRID_SIZE_ATTR_INDEX = 16,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_X_ATTR_INDEX = 17,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Y_ATTR_INDEX = 18,
  LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Z_ATTR_INDEX = 19,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_GENERIC_ATTR_INDEX = 20,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_GLOBAL_ATTR_INDEX = 21,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_WORKGROUP_ATTR_INDEX = 22,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_CONSTANT_ATTR_INDEX = 23,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_PRIVATE_ATTR_INDEX = 24,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_HOST_ATTR_INDEX = 25,
  LOOM_AMDGPU_TARGET_MEMORY_SPACE_DESCRIPTOR_ATTR_INDEX = 26,
  LOOM_AMDGPU_TARGET_ABI_ATTR_INDEX = 27,
  LOOM_AMDGPU_TARGET_EXPORT_SYMBOL_ATTR_INDEX = 28,
  LOOM_AMDGPU_TARGET_LINKAGE_ATTR_INDEX = 29,
  LOOM_AMDGPU_TARGET_CONTRACT_SET_KEY_ATTR_INDEX = 30,
  LOOM_AMDGPU_TARGET_CONTRACT_FEATURE_BITS_ATTR_INDEX = 31,
  LOOM_AMDGPU_TARGET_FEATURES_ATTR_INDEX = 32,
};
LOOM_DEFINE_ATTR_SYMBOL(loom_amdgpu_target_symbol, LOOM_AMDGPU_TARGET_SYMBOL_ATTR_INDEX)
#define loom_amdgpu_target_rewrite_symbol(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_SYMBOL_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_amdgpu_target_kind, LOOM_AMDGPU_TARGET_KIND_ATTR_INDEX, loom_amdgpu_target_kind_t)
#define loom_amdgpu_target_rewrite_kind(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_KIND_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_amdgpu_target_codegen_format, LOOM_AMDGPU_TARGET_CODEGEN_FORMAT_ATTR_INDEX, loom_target_codegen_format_t)
#define loom_amdgpu_target_has_codegen_format(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_CODEGEN_FORMAT_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_codegen_format(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_CODEGEN_FORMAT_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_amdgpu_target_artifact_format, LOOM_AMDGPU_TARGET_ARTIFACT_FORMAT_ATTR_INDEX, loom_target_artifact_format_t)
#define loom_amdgpu_target_has_artifact_format(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_ARTIFACT_FORMAT_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_artifact_format(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_ARTIFACT_FORMAT_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_default_pointer_bitwidth, LOOM_AMDGPU_TARGET_DEFAULT_POINTER_BITWIDTH_ATTR_INDEX)
#define loom_amdgpu_target_has_default_pointer_bitwidth(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_DEFAULT_POINTER_BITWIDTH_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_default_pointer_bitwidth(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_DEFAULT_POINTER_BITWIDTH_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_index_bitwidth, LOOM_AMDGPU_TARGET_INDEX_BITWIDTH_ATTR_INDEX)
#define loom_amdgpu_target_has_index_bitwidth(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_INDEX_BITWIDTH_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_index_bitwidth(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_INDEX_BITWIDTH_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_offset_bitwidth, LOOM_AMDGPU_TARGET_OFFSET_BITWIDTH_ATTR_INDEX)
#define loom_amdgpu_target_has_offset_bitwidth(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_OFFSET_BITWIDTH_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_offset_bitwidth(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_OFFSET_BITWIDTH_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_size_x, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_X_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_size_x(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_X_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_size_x(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_X_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_size_y, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Y_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_size_y(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Y_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_size_y(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Y_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_size_z, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Z_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_size_z(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Z_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_size_z(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_SIZE_Z_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_flat_workgroup_size, LOOM_AMDGPU_TARGET_MAX_FLAT_WORKGROUP_SIZE_ATTR_INDEX)
#define loom_amdgpu_target_has_max_flat_workgroup_size(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_FLAT_WORKGROUP_SIZE_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_flat_workgroup_size(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_FLAT_WORKGROUP_SIZE_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_storage_bytes, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_STORAGE_BYTES_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_storage_bytes(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_STORAGE_BYTES_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_storage_bytes(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_STORAGE_BYTES_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_subgroup_size, LOOM_AMDGPU_TARGET_SUBGROUP_SIZE_ATTR_INDEX)
#define loom_amdgpu_target_has_subgroup_size(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_SUBGROUP_SIZE_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_subgroup_size(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_SUBGROUP_SIZE_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_grid_size_x, LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_X_ATTR_INDEX)
#define loom_amdgpu_target_has_max_grid_size_x(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_X_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_grid_size_x(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_X_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_grid_size_y, LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Y_ATTR_INDEX)
#define loom_amdgpu_target_has_max_grid_size_y(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Y_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_grid_size_y(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Y_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_grid_size_z, LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Z_ATTR_INDEX)
#define loom_amdgpu_target_has_max_grid_size_z(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Z_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_grid_size_z(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_GRID_SIZE_Z_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_flat_grid_size, LOOM_AMDGPU_TARGET_MAX_FLAT_GRID_SIZE_ATTR_INDEX)
#define loom_amdgpu_target_has_max_flat_grid_size(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_FLAT_GRID_SIZE_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_flat_grid_size(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_FLAT_GRID_SIZE_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_count_x, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_X_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_count_x(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_X_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_count_x(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_X_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_count_y, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Y_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_count_y(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Y_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_count_y(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Y_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_max_workgroup_count_z, LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Z_ATTR_INDEX)
#define loom_amdgpu_target_has_max_workgroup_count_z(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Z_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_max_workgroup_count_z(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MAX_WORKGROUP_COUNT_Z_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_generic, LOOM_AMDGPU_TARGET_MEMORY_SPACE_GENERIC_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_generic(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_GENERIC_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_generic(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_GENERIC_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_global, LOOM_AMDGPU_TARGET_MEMORY_SPACE_GLOBAL_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_global(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_GLOBAL_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_global(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_GLOBAL_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_workgroup, LOOM_AMDGPU_TARGET_MEMORY_SPACE_WORKGROUP_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_workgroup(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_WORKGROUP_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_workgroup(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_WORKGROUP_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_constant, LOOM_AMDGPU_TARGET_MEMORY_SPACE_CONSTANT_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_constant(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_CONSTANT_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_constant(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_CONSTANT_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_private, LOOM_AMDGPU_TARGET_MEMORY_SPACE_PRIVATE_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_private(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_PRIVATE_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_private(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_PRIVATE_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_host, LOOM_AMDGPU_TARGET_MEMORY_SPACE_HOST_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_host(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_HOST_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_host(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_HOST_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_memory_space_descriptor, LOOM_AMDGPU_TARGET_MEMORY_SPACE_DESCRIPTOR_ATTR_INDEX)
#define loom_amdgpu_target_has_memory_space_descriptor(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_MEMORY_SPACE_DESCRIPTOR_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_memory_space_descriptor(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_MEMORY_SPACE_DESCRIPTOR_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_amdgpu_target_abi, LOOM_AMDGPU_TARGET_ABI_ATTR_INDEX, loom_target_abi_kind_t)
#define loom_amdgpu_target_has_abi(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_ABI_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_abi(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_ABI_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_STRING(loom_amdgpu_target_export_symbol, LOOM_AMDGPU_TARGET_EXPORT_SYMBOL_ATTR_INDEX)
#define loom_amdgpu_target_has_export_symbol(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_EXPORT_SYMBOL_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_export_symbol(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_EXPORT_SYMBOL_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_amdgpu_target_linkage, LOOM_AMDGPU_TARGET_LINKAGE_ATTR_INDEX, loom_target_linkage_t)
#define loom_amdgpu_target_has_linkage(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_LINKAGE_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_linkage(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_LINKAGE_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_STRING(loom_amdgpu_target_contract_set_key, LOOM_AMDGPU_TARGET_CONTRACT_SET_KEY_ATTR_INDEX)
#define loom_amdgpu_target_has_contract_set_key(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_CONTRACT_SET_KEY_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_contract_set_key(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_CONTRACT_SET_KEY_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_I64(loom_amdgpu_target_contract_feature_bits, LOOM_AMDGPU_TARGET_CONTRACT_FEATURE_BITS_ATTR_INDEX)
#define loom_amdgpu_target_has_contract_feature_bits(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_CONTRACT_FEATURE_BITS_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_contract_feature_bits(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_CONTRACT_FEATURE_BITS_ATTR_INDEX, (attribute))
LOOM_DEFINE_ATTR_SIGNED_ENUM_SET(loom_amdgpu_target_features, LOOM_AMDGPU_TARGET_FEATURES_ATTR_INDEX)
#define loom_amdgpu_target_has_features(op) \
  (!loom_attr_is_absent(loom_op_const_attrs((op))[LOOM_AMDGPU_TARGET_FEATURES_ATTR_INDEX]))
#define loom_amdgpu_target_rewrite_features(rewriter, op, attribute) \
  loom_rewriter_set_attr((rewriter), (op), LOOM_AMDGPU_TARGET_FEATURES_ATTR_INDEX, (attribute))
enum loom_amdgpu_target_build_flag_bits_e {
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_CODEGEN_FORMAT = 1u << 0,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_ARTIFACT_FORMAT = 1u << 1,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_DEFAULT_POINTER_BITWIDTH = 1u << 2,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_INDEX_BITWIDTH = 1u << 3,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_OFFSET_BITWIDTH = 1u << 4,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_SIZE_X = 1u << 5,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_SIZE_Y = 1u << 6,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_SIZE_Z = 1u << 7,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_FLAT_WORKGROUP_SIZE = 1u << 8,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_STORAGE_BYTES = 1u << 9,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_SUBGROUP_SIZE = 1u << 10,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_GRID_SIZE_X = 1u << 11,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_GRID_SIZE_Y = 1u << 12,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_GRID_SIZE_Z = 1u << 13,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_FLAT_GRID_SIZE = 1u << 14,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_COUNT_X = 1u << 15,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_COUNT_Y = 1u << 16,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MAX_WORKGROUP_COUNT_Z = 1u << 17,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_GENERIC = 1u << 18,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_GLOBAL = 1u << 19,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_WORKGROUP = 1u << 20,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_CONSTANT = 1u << 21,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_PRIVATE = 1u << 22,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_HOST = 1u << 23,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_MEMORY_SPACE_DESCRIPTOR = 1u << 24,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_ABI = 1u << 25,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_EXPORT_SYMBOL = 1u << 26,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_LINKAGE = 1u << 27,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_CONTRACT_SET_KEY = 1u << 28,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_CONTRACT_FEATURE_BITS = 1u << 29,
  LOOM_AMDGPU_TARGET_BUILD_FLAG_HAS_FEATURES = 1u << 30,
};
typedef uint32_t loom_amdgpu_target_build_flags_t;
iree_status_t loom_amdgpu_target_build(
    loom_builder_t* builder,
    loom_amdgpu_target_build_flags_t build_flags,
    loom_amdgpu_target_kind_t kind,
    loom_symbol_ref_t symbol,
    loom_optional uint8_t codegen_format,
    loom_optional uint8_t artifact_format,
    loom_optional int64_t default_pointer_bitwidth,
    loom_optional int64_t index_bitwidth,
    loom_optional int64_t offset_bitwidth,
    loom_optional int64_t max_workgroup_size_x,
    loom_optional int64_t max_workgroup_size_y,
    loom_optional int64_t max_workgroup_size_z,
    loom_optional int64_t max_flat_workgroup_size,
    loom_optional int64_t max_workgroup_storage_bytes,
    loom_optional int64_t subgroup_size,
    loom_optional int64_t max_grid_size_x,
    loom_optional int64_t max_grid_size_y,
    loom_optional int64_t max_grid_size_z,
    loom_optional int64_t max_flat_grid_size,
    loom_optional int64_t max_workgroup_count_x,
    loom_optional int64_t max_workgroup_count_y,
    loom_optional int64_t max_workgroup_count_z,
    loom_optional int64_t memory_space_generic,
    loom_optional int64_t memory_space_global,
    loom_optional int64_t memory_space_workgroup,
    loom_optional int64_t memory_space_constant,
    loom_optional int64_t memory_space_private,
    loom_optional int64_t memory_space_host,
    loom_optional int64_t memory_space_descriptor,
    loom_optional uint8_t abi,
    loom_optional loom_string_id_t export_symbol,
    loom_optional uint8_t linkage,
    loom_optional loom_string_id_t contract_set_key,
    loom_optional int64_t contract_feature_bits,
    loom_optional loom_signed_enum_set_t features,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_amdgpu_target_record_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// Returns the vtable array for the amdgpu dialect.
const loom_op_vtable_t* const* loom_amdgpu_dialect_vtables(
    iree_host_size_t* out_count);

// Returns the dense semantic metadata array for the amdgpu dialect.
const loom_op_semantics_t* loom_amdgpu_dialect_op_semantics(
    iree_host_size_t* out_count);

// Returns semantic metadata for a amdgpu op kind, or empty metadata.
loom_op_semantics_t loom_amdgpu_op_semantics(
    loom_op_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_OPS_AMDGPU_OPS_H_
