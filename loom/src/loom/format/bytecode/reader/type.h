// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared type payload decoding and canonical IR materialization.

#ifndef LOOM_FORMAT_BYTECODE_READER_TYPE_H_
#define LOOM_FORMAT_BYTECODE_READER_TYPE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/format/bytecode/reader/decoder.h"
#include "loom/format/bytecode/reader/type_plan.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ir/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_bytecode_reader_module_view_t
    loom_bytecode_reader_module_view_t;

// State required to materialize a validated type plan into a module.
typedef struct loom_bytecode_type_materializer_t {
  // Bounded decoder and structured diagnostic state.
  loom_bytecode_reader_decoder_t* decoder;
  // Full input bytes containing retained parameterized-type spans.
  iree_const_byte_span_t bytecode;
  // Finalized type and parameterized-type registry context.
  loom_context_t* context;
  // Immutable validated module facts containing the type plan.
  const loom_bytecode_reader_module_view_t* module_view;
  // Resettable scratch storage for composite type construction.
  iree_arena_allocator_t* scratch_arena;
  // Module receiving canonical type-table entries.
  loom_module_t* output_module;
} loom_bytecode_type_materializer_t;

// Validates a wire kind and maps it to the independent native type kind.
iree_status_t loom_bytecode_type_decode_kind(
    loom_bytecode_reader_decoder_t* decoder, uint8_t kind_byte, uint64_t offset,
    loom_type_kind_t* out_kind);

// Reads and validates the two target register carrier words shared by static
// and scoped type records. |type_index| identifies the record in diagnostics.
iree_status_t loom_bytecode_type_read_register_carrier(
    loom_bytecode_reader_decoder_t* decoder,
    loom_bytecode_reader_cursor_t* cursor, uint64_t type_index,
    uint64_t out_payload[2]);

// Interns a structural type from validated parent metadata and canonical child
// IDs. No temporary child payload is assembled. Selected readers project the
// plan's name and dependency IDs into |module| before construction. The result
// owns its payload independently of the plan and fact lifetimes.
iree_status_t loom_bytecode_type_materialize_structural(
    const loom_bytecode_structural_type_plan_t* plan,
    const loom_bytecode_structural_type_fact_t* fact,
    const loom_type_id_t* dependency_ids, loom_module_t* module,
    loom_type_id_t* out_type_id);

// Consumes a validated plan in source order, replacing each entry with its
// canonical output identity. Sparse structural child slots are consumed too.
iree_status_t loom_bytecode_type_materialize(
    loom_bytecode_type_materializer_t* materializer);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_BYTECODE_READER_TYPE_H_
