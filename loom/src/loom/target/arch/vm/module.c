// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/module.h"

#include <stdlib.h>

#include "iree/io/vec_stream.h"
#include "iree/vm/bytecode/wire/module.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/vm/function.h"
#include "loom/target/function_version.h"

typedef struct loom_vm_module_function_t {
  // Borrowed executable function and signature values.
  loom_func_like_t function;
  // Function target facts retained by the shared specialization pipeline.
  const loom_target_facts_t* target_facts;
  // Source-ordered entry argument IDs in the module.
  const loom_value_id_t* arguments;
  // Source-ordered signature result IDs in the module.
  loom_value_slice_t results;
  // Public name, or empty for an internal function.
  iree_string_view_t export_name;
  // Function ordinal in the emitted image.
  uint16_t ordinal;
  // Source-ordered logical argument count.
  uint16_t argument_count;
  // Canonical callable ordinal assigned by signature sorting.
  uint16_t callable_ordinal;
  // Exact logical fields and their physical argument/result bank counts.
  loom_vm_function_signature_t signature;
} loom_vm_module_function_t;

typedef struct loom_vm_module_function_span_t {
  // Arena-owned function records in bytecode ordinal order.
  loom_vm_module_function_t* values;
  // Symbol-indexed local function ordinals; UINT16_MAX marks other symbols.
  uint16_t* ordinals_by_symbol;
  // Number of records in |values|, bounded by the module symbol ID space.
  uint32_t count;
  // Whether any signature names the Core buffer reference type.
  bool uses_buffer_type;
} loom_vm_module_function_span_t;

// A signature is ordered by argument count/types then result
// count/types, exactly as the wire callable table requires. Ordinals are
// assigned by sorting once; the runtime performs no hashing or interning.
static int loom_vm_signature_compare(const void* lhs_ptr, const void* rhs_ptr) {
  const loom_vm_module_function_t* lhs =
      *(const loom_vm_module_function_t* const*)lhs_ptr;
  const loom_vm_module_function_t* rhs =
      *(const loom_vm_module_function_t* const*)rhs_ptr;
  int comparison = (int)lhs->argument_count - (int)rhs->argument_count;
  if (comparison) return comparison;
  for (uint16_t i = 0; i < lhs->argument_count; ++i) {
    comparison = (int)lhs->signature.fields[i].kind_u16 -
                 (int)rhs->signature.fields[i].kind_u16;
    if (comparison) return comparison;
  }
  comparison = (int)lhs->results.count - (int)rhs->results.count;
  if (comparison) return comparison;
  for (uint16_t i = 0; i < lhs->results.count; ++i) {
    comparison = (int)lhs->signature.fields[lhs->argument_count + i].kind_u16 -
                 (int)rhs->signature.fields[rhs->argument_count + i].kind_u16;
    if (comparison) return comparison;
  }
  return 0;
}

static int loom_vm_export_compare(const void* lhs_ptr, const void* rhs_ptr) {
  const loom_vm_module_function_t* lhs =
      *(const loom_vm_module_function_t* const*)lhs_ptr;
  const loom_vm_module_function_t* rhs =
      *(const loom_vm_module_function_t* const*)rhs_ptr;
  return iree_string_view_compare(lhs->export_name, rhs->export_name);
}

static iree_status_t loom_vm_signature_type(loom_type_t type,
                                            uint16_t* out_kind) {
  // Logical scalar tags are stable, small, and independent of cell width.
  // Predicates cross the ABI as canonical zero/one i32 values.
  static const uint8_t kScalarKinds[LOOM_SCALAR_TYPE_COUNT_] = {
      [LOOM_SCALAR_TYPE_I1] = IREE_VM_BYTECODE_SIGNATURE_KIND_I32,
      [LOOM_SCALAR_TYPE_I8] = IREE_VM_BYTECODE_SIGNATURE_KIND_I8,
      [LOOM_SCALAR_TYPE_I16] = IREE_VM_BYTECODE_SIGNATURE_KIND_I16,
      [LOOM_SCALAR_TYPE_I32] = IREE_VM_BYTECODE_SIGNATURE_KIND_I32,
      [LOOM_SCALAR_TYPE_I64] = IREE_VM_BYTECODE_SIGNATURE_KIND_I64,
      [LOOM_SCALAR_TYPE_F8E4M3] = IREE_VM_BYTECODE_SIGNATURE_KIND_F8E4M3FN,
      [LOOM_SCALAR_TYPE_F8E5M2] = IREE_VM_BYTECODE_SIGNATURE_KIND_F8E5M2,
      [LOOM_SCALAR_TYPE_F16] = IREE_VM_BYTECODE_SIGNATURE_KIND_F16,
      [LOOM_SCALAR_TYPE_BF16] = IREE_VM_BYTECODE_SIGNATURE_KIND_BF16,
      [LOOM_SCALAR_TYPE_F32] = IREE_VM_BYTECODE_SIGNATURE_KIND_F32,
      [LOOM_SCALAR_TYPE_F64] = IREE_VM_BYTECODE_SIGNATURE_KIND_F64,
  };
  const loom_type_t* value_type = loom_type_register_value_type(type);
  if (value_type) {
    if (loom_type_is_buffer(*value_type)) {
      *out_kind = IREE_VM_BYTECODE_SIGNATURE_KIND_REF;
      return iree_ok_status();
    }
    const uint8_t kind = kScalarKinds[loom_type_element_type(*value_type)];
    if (kind) {
      *out_kind = kind;
      return iree_ok_status();
    }
  }
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "VM signature requires a supported typed register");
}

// Only top-level symbol definitions are collected. Callgraph specialization
// and library composition belong to the shared compiler, not this writer.
static iree_status_t loom_vm_module_collect(
    const loom_target_emit_request_t* request,
    loom_vm_module_function_span_t* out_functions) {
  const loom_module_t* module = request->module;
  loom_target_function_version_snapshot_t versions = {0};
  IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
      module, request->function_versions, request->scratch_arena, &versions));
  loom_vm_module_function_t* functions = NULL;
  iree_host_size_t storage_size = 0;
  iree_host_size_t ordinals_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      0, &storage_size,
      IREE_STRUCT_FIELD(module->symbols.count, loom_vm_module_function_t, NULL),
      IREE_STRUCT_FIELD(module->symbols.count, uint16_t, &ordinals_offset)));
  IREE_RETURN_IF_ERROR(iree_arena_allocate(request->scratch_arena, storage_size,
                                           (void**)&functions));
  uint16_t* ordinals_by_symbol =
      (uint16_t*)((uint8_t*)functions + ordinals_offset);
  uint32_t count = 0;
  iree_host_size_t descriptor_count = 0;
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < module->symbols.count && iree_status_is_ok(status);
       ++i) {
    const loom_symbol_t* symbol = &module->symbols.entries[i];
    loom_op_t* op = symbol->defining_op;
    ordinals_by_symbol[i] = UINT16_MAX;
    if (!loom_low_func_def_isa(op)) continue;
    loom_func_like_t function = loom_func_like_cast(module, op);
    const loom_string_id_t contract = loom_func_like_repr_contract(function);
    if (contract == LOOM_STRING_ID_INVALID ||
        !iree_string_view_equal(module->strings.entries[contract],
                                IREE_SV("vm.core"))) {
      continue;
    }
    loom_vm_module_function_t* entry = &functions[count];
    ordinals_by_symbol[i] = (uint16_t)count;
    *entry = (loom_vm_module_function_t){
        .function = function,
        .target_facts = loom_target_function_version_target_facts(
            loom_target_function_version_snapshot_handle_at(&versions, i)),
        .ordinal = (uint16_t)count,
        .results = loom_low_func_def_results(op),
    };
    entry->arguments = loom_func_like_arg_ids(function, &entry->argument_count);
    const loom_string_id_t export_name = loom_func_like_export_symbol(function);
    if (export_name != LOOM_STRING_ID_INVALID) {
      entry->export_name = module->strings.entries[export_name];
      if (iree_string_view_is_empty(entry->export_name)) {
        status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                  "VM export names must not be empty");
      }
    } else if (loom_func_like_visibility(function)) {
      entry->export_name = module->strings.entries[symbol->name_id];
    }
    descriptor_count += entry->argument_count + entry->results.count;
    ++count;
  }
  IREE_RETURN_IF_ERROR(status);
  if (!count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "module contains no VM function definitions");
  }
  iree_vm_bytecode_v0_signature_descriptor_row_t* descriptors = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, iree_max(descriptor_count, 1),
      sizeof(*descriptors), (void**)&descriptors));
  bool uses_buffer_type = false;
  for (uint32_t i = 0; i < count && iree_status_is_ok(status); ++i) {
    loom_vm_module_function_t* entry = &functions[i];
    entry->signature.fields = descriptors;
    const uint32_t field_count = entry->argument_count + entry->results.count;
    for (uint32_t j = 0; j < field_count && iree_status_is_ok(status); ++j) {
      const loom_value_id_t value =
          j < entry->argument_count
              ? entry->arguments[j]
              : entry->results.values[j - entry->argument_count];
      descriptors[j].type_ordinal_u16 = 0;
      status = loom_vm_signature_type(loom_module_value_type(module, value),
                                      &descriptors[j].kind_u16);
      if (iree_status_is_ok(status)) {
        iree_vm_bytecode_v0_signature_row_t* row = &entry->signature.row;
        uint16_t* bank_count;
        if (descriptors[j].kind_u16 == IREE_VM_BYTECODE_SIGNATURE_KIND_REF) {
          uses_buffer_type = true;
          bank_count = j < entry->argument_count ? &row->argument_ref_count_u16
                                                 : &row->result_ref_count_u16;
        } else {
          bank_count = j < entry->argument_count
                           ? &row->argument_value_count_u16
                           : &row->result_value_count_u16;
        }
        ++(*bank_count);
      }
    }
    descriptors += field_count;
  }
  if (iree_status_is_ok(status)) {
    *out_functions = (loom_vm_module_function_span_t){
        .values = functions,
        .ordinals_by_symbol = ordinals_by_symbol,
        .count = count,
        .uses_buffer_type = uses_buffer_type,
    };
  }
  return status;
}

static iree_status_t loom_vm_section_begin(
    iree_io_stream_t* stream, iree_vm_bytecode_section_type_t type,
    iree_vm_bytecode_v0_section_directory_row_t* row,
    iree_io_stream_pos_t* out_start) {
  const uint8_t zero = 0;
  const iree_io_stream_pos_t padding =
      -iree_io_stream_offset(stream) & (IREE_VM_BYTECODE_IMAGE_ALIGNMENT - 1);
  IREE_RETURN_IF_ERROR(iree_io_stream_fill(stream, padding, &zero, 1));
  *out_start = iree_io_stream_offset(stream);
  *row = (iree_vm_bytecode_v0_section_directory_row_t){
      .section_type_u16 = type,
      .payload_alignment_u32 = IREE_VM_BYTECODE_IMAGE_ALIGNMENT,
  };
  return iree_ok_status();
}

// Patches reserved bytes without changing the append position or requiring
// contiguous stream storage.
static iree_status_t loom_vm_stream_patch(iree_io_stream_t* stream,
                                          iree_io_stream_pos_t offset,
                                          iree_const_byte_span_t contents) {
  const iree_io_stream_pos_t end = iree_io_stream_offset(stream);
  IREE_RETURN_IF_ERROR(
      iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, offset));
  IREE_RETURN_IF_ERROR(
      iree_io_stream_write(stream, contents.data_length, contents.data));
  return iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, end);
}

static iree_status_t loom_vm_module_write(
    const loom_target_emit_request_t* request,
    loom_vm_module_function_span_t functions, iree_io_stream_t* stream) {
  const uint32_t function_count = functions.count;
  // Separate sorted views preserve function ordinals and avoid sorting exports
  // again when emitting their rows after the callable table.
  loom_vm_module_function_t** sorted = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(request->scratch_arena, 2 * function_count,
                                sizeof(*sorted), (void**)&sorted));
  loom_vm_module_function_t** exports = sorted + function_count;
  uint32_t export_count = 0;
  for (uint32_t i = 0; i < function_count; ++i) {
    sorted[i] = &functions.values[i];
    if (!iree_string_view_is_empty(functions.values[i].export_name)) {
      exports[export_count++] = &functions.values[i];
    }
  }
  const iree_vm_bytecode_v0_image_header_t header = {
      .magic_u8 = {'I', 'R', 'E', 'E', 'V', 'M', 0, 0},
      .core_major_u16 = IREE_VM_BYTECODE_CORE_MAJOR,
      .core_required_minor_u16 = IREE_VM_BYTECODE_CORE_MINOR,
      .section_count_u16 = 3 + (export_count != 0) +
                           (export_count != 0 || functions.uses_buffer_type) +
                           functions.uses_buffer_type,
  };
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  iree_vm_bytecode_v0_section_directory_row_t directory[6] = {0};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, header.section_count_u16 * sizeof(directory[0]), directory));
  uint16_t section = 0;
  iree_io_stream_pos_t start = 0;
  iree_status_t status = iree_ok_status();
  if (export_count || functions.uses_buffer_type) {
    qsort(exports, export_count, sizeof(*exports), loom_vm_export_compare);
    IREE_RETURN_IF_ERROR(loom_vm_section_begin(
        stream, IREE_VM_BYTECODE_SECTION_STRINGS, &directory[section], &start));
    const iree_vm_bytecode_v0_strings_header_t strings_header = {
        export_count + (functions.uses_buffer_type ? 2 : 0)};
    if (strings_header.string_count_u32 > UINT16_MAX) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "VM string count exceeds the u16 ordinal space");
    }
    IREE_RETURN_IF_ERROR(
        iree_io_stream_write(stream, sizeof(strings_header), &strings_header));
    uint32_t offset = 0;
    IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(offset), &offset));
    for (uint32_t i = 0; i < export_count && iree_status_is_ok(status); ++i) {
      if (exports[i]->export_name.size > UINT32_MAX - offset) {
        status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                  "VM string bytes exceed u32");
      } else if (i && iree_string_view_equal(exports[i - 1]->export_name,
                                             exports[i]->export_name)) {
        status = iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT, "duplicate VM export '%.*s'",
            (int)exports[i]->export_name.size, exports[i]->export_name.data);
      } else {
        offset += (uint32_t)exports[i]->export_name.size;
        status = iree_io_stream_write(stream, sizeof(offset), &offset);
      }
    }
    // Source buffer types lower to the single Core vm.buffer type. These two
    // strings follow export names, preserving their direct ordinal mapping.
    if (functions.uses_buffer_type && iree_status_is_ok(status)) {
      if (offset > UINT32_MAX - 8) {
        status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                  "VM string bytes exceed u32");
      } else {
        const uint32_t offsets[] = {offset + 2, offset + 8};
        status = iree_io_stream_write(stream, sizeof(offsets), offsets);
      }
    }
    for (uint32_t i = 0; i < export_count && iree_status_is_ok(status); ++i) {
      status = iree_io_stream_write_string(stream, exports[i]->export_name);
    }
    if (functions.uses_buffer_type && iree_status_is_ok(status)) {
      status = iree_io_stream_write_string(stream, IREE_SV("vmbuffer"));
    }
    IREE_RETURN_IF_ERROR(status);
    directory[section++].byte_length_u64 =
        iree_io_stream_offset(stream) - start;
  }

  if (functions.uses_buffer_type) {
    IREE_RETURN_IF_ERROR(
        loom_vm_section_begin(stream, IREE_VM_BYTECODE_SECTION_REF_TYPES,
                              &directory[section], &start));
    const iree_vm_bytecode_v0_ref_types_header_t types_header = {
        .group_count_u32 = 1};
    const iree_vm_bytecode_v0_ref_type_group_row_t group = {
        .namespace_string_u16 = (uint16_t)export_count, .entry_count_u32 = 1};
    const iree_vm_bytecode_v0_ref_type_entry_row_t entry = {
        .type_name_string_u16 = (uint16_t)(export_count + 1)};
    IREE_RETURN_IF_ERROR(
        iree_io_stream_write(stream, sizeof(types_header), &types_header));
    IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(group), &group));
    IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(entry), &entry));
    directory[section++].byte_length_u64 =
        iree_io_stream_offset(stream) - start;
  }

  qsort(sorted, function_count, sizeof(*sorted), loom_vm_signature_compare);
  uint32_t callable_count = 0;
  for (uint32_t i = 0; i < function_count; ++i) {
    if (!callable_count ||
        loom_vm_signature_compare(&sorted[callable_count - 1], &sorted[i])) {
      sorted[callable_count++] = sorted[i];
    }
    sorted[i]->callable_ordinal = (uint16_t)(callable_count - 1);
  }
  IREE_RETURN_IF_ERROR(
      loom_vm_section_begin(stream, IREE_VM_BYTECODE_SECTION_SIGNATURES,
                            &directory[section], &start));
  const iree_vm_bytecode_v0_signatures_header_t signatures_header = {
      callable_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(signatures_header),
                                            &signatures_header));
  uint32_t descriptor_base = 0;
  for (uint32_t i = 0; i < callable_count && iree_status_is_ok(status); ++i) {
    const loom_vm_module_function_t* entry = sorted[i];
    iree_vm_bytecode_v0_signature_row_t signature = entry->signature.row;
    signature.descriptor_base_u32 = descriptor_base;
    status = iree_io_stream_write(stream, sizeof(signature), &signature);
    descriptor_base += entry->argument_count + entry->results.count;
  }
  for (uint32_t i = 0; i < callable_count && iree_status_is_ok(status); ++i) {
    const loom_vm_module_function_t* entry = sorted[i];
    status =
        iree_io_stream_write(stream,
                             (entry->argument_count + entry->results.count) *
                                 sizeof(*entry->signature.fields),
                             entry->signature.fields);
  }
  IREE_RETURN_IF_ERROR(status);
  directory[section++].byte_length_u64 = iree_io_stream_offset(stream) - start;

  IREE_RETURN_IF_ERROR(
      loom_vm_section_begin(stream, IREE_VM_BYTECODE_SECTION_CALLABLE_TYPES,
                            &directory[section], &start));
  const iree_vm_bytecode_v0_callable_types_header_t callables_header = {
      callable_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(callables_header),
                                            &callables_header));
  for (uint32_t i = 0; i < callable_count && iree_status_is_ok(status); ++i) {
    const iree_vm_bytecode_v0_callable_type_row_t callable = {
        .signature_ordinal_u16 = (uint16_t)i};
    status = iree_io_stream_write(stream, sizeof(callable), &callable);
  }
  IREE_RETURN_IF_ERROR(status);
  directory[section++].byte_length_u64 = iree_io_stream_offset(stream) - start;

  if (export_count) {
    IREE_RETURN_IF_ERROR(loom_vm_section_begin(
        stream, IREE_VM_BYTECODE_SECTION_EXPORTS, &directory[section], &start));
    const iree_vm_bytecode_v0_exports_header_t exports_header = {export_count};
    IREE_RETURN_IF_ERROR(
        iree_io_stream_write(stream, sizeof(exports_header), &exports_header));
    for (uint32_t i = 0; i < export_count && iree_status_is_ok(status); ++i) {
      const loom_vm_module_function_t* entry = exports[i];
      const iree_vm_bytecode_v0_export_row_t row = {
          .name_string_u16 = (uint16_t)i,
          .callable_type_ordinal_u16 = entry->callable_ordinal,
          .function_ordinal_u16 = entry->ordinal,
      };
      status = iree_io_stream_write(stream, sizeof(row), &row);
    }
    IREE_RETURN_IF_ERROR(status);
    directory[section++].byte_length_u64 =
        iree_io_stream_offset(stream) - start;
  }
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_FUNCTIONS, &directory[section], &start));
  iree_vm_bytecode_v0_functions_header_t functions_header = {
      .function_count_u32 = function_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(functions_header),
                                            &functions_header));
  const uint8_t zero = 0;
  IREE_RETURN_IF_ERROR(iree_io_stream_fill(
      stream, function_count * sizeof(iree_vm_bytecode_v0_function_row_t),
      &zero, 1));
  const iree_io_stream_pos_t bytecode_base = iree_io_stream_offset(stream);
  for (uint32_t i = 0; i < function_count && iree_status_is_ok(status); ++i) {
    const iree_io_stream_pos_t offset =
        iree_io_stream_offset(stream) - bytecode_base;
    if (offset > UINT32_MAX) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "VM bytecode offset exceeds u32");
      continue;
    }
    iree_vm_bytecode_v0_function_row_t row = {
        .callable_type_ordinal_u16 = functions.values[i].callable_ordinal,
        .bytecode_offset_u32 = (uint32_t)offset,
    };
    status = loom_vm_function_emit(request, functions.values[i].function,
                                   functions.values[i].target_facts,
                                   &functions.values[i].signature,
                                   functions.ordinals_by_symbol, stream, &row);
    if (iree_status_is_ok(status)) {
      functions_header.maximum_block_count_u32 = iree_max(
          functions_header.maximum_block_count_u32, row.block_count_u32);
      status = loom_vm_stream_patch(
          stream, start + sizeof(functions_header) + i * sizeof(row),
          iree_make_const_byte_span(&row, sizeof(row)));
    }
  }
  IREE_RETURN_IF_ERROR(status);
  directory[section].byte_length_u64 = iree_io_stream_offset(stream) - start;
  IREE_RETURN_IF_ERROR(loom_vm_stream_patch(
      stream, start,
      iree_make_const_byte_span(&functions_header, sizeof(functions_header))));
  return loom_vm_stream_patch(
      stream, sizeof(header),
      iree_make_const_byte_span(
          directory, header.section_count_u16 * sizeof(directory[0])));
}

iree_status_t loom_vm_module_emit(const loom_target_emit_request_t* request,
                                  loom_target_emit_artifact_t* out_artifact) {
  *out_artifact = (loom_target_emit_artifact_t){0};
  loom_vm_module_function_span_t functions = {0};
  IREE_RETURN_IF_ERROR(loom_vm_module_collect(request, &functions));
  iree_io_stream_t* stream = NULL;
  IREE_RETURN_IF_ERROR(iree_io_vec_stream_create(
      IREE_IO_STREAM_MODE_WRITABLE | IREE_IO_STREAM_MODE_SEEKABLE, 32 * 1024,
      request->allocator, &stream));
  iree_status_t status = loom_vm_module_write(request, functions, stream);
  if (iree_status_is_ok(status)) {
    status = iree_io_vec_stream_move_contents(stream, &out_artifact->contents);
  }
  if (iree_status_is_ok(status)) {
    out_artifact->target_artifact_format =
        LOOM_TARGET_ARTIFACT_FORMAT_VM_BINARY;
  }
  iree_io_stream_release(stream);
  return status;
}
