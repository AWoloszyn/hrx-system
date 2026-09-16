// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Retained dependencies of structured loop scheduling units.

#ifndef LOOM_TRANSFORMS_SCF_SCF_BODY_H_
#define LOOM_TRANSFORMS_SCF_SCF_BODY_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t loom_scf_body_effect_flags_t;
enum loom_scf_body_effect_flag_bits_e {
  LOOM_SCF_BODY_EFFECT_READ = 1u << 0,
  LOOM_SCF_BODY_EFFECT_WRITE = 1u << 1,
  LOOM_SCF_BODY_EFFECT_ORDERED = 1u << 2,
  // A read effect that is not an ordinary memory load.
  LOOM_SCF_BODY_EFFECT_NON_LOAD_READ = 1u << 3,
};

typedef struct loom_scf_body_reference_t {
  // Body-local value required before materializing the referencing operation.
  loom_value_id_t value_id;
  // A detached definition only requires a mapping to exist. A body-local
  // definition requires a distinct value for the materialized iteration.
  bool allow_identity_mapping;
} loom_scf_body_reference_t;

typedef struct loom_scf_body_operation_t {
  // Borrowed source operation, including its complete verified payload.
  const loom_op_t* op;
  // First dependency in the body's packed reference array.
  iree_host_size_t reference_begin;
  // Number of outer-body dependencies, including nested region captures.
  iree_host_size_t reference_count;
  // Combined effects governing whether different iterations may commute.
  loom_scf_body_effect_flags_t effects;
  // Number of ordinary load operations, including loads inside nested regions.
  uint32_t load_count;
} loom_scf_body_operation_t;

typedef struct loom_scf_body_t {
  // Source operations in authored order, excluding the terminator.
  loom_scf_body_operation_t* operations;
  // Number of source operations.
  uint32_t count;
  // Complete local payload dependencies, grouped by operation.
  loom_scf_body_reference_t* references;
  // Number of entries in references.
  iree_host_size_t reference_count;
  // Terminator payload dependencies, used when forwarding carried state.
  loom_scf_body_operation_t terminator;
} loom_scf_body_t;

// Captures the verified |block|'s live operations, source effects and complete
// local SSA dependencies in one traversal. A structured if/for and its
// regions form one scheduling unit; their outer-body captures and effects are
// retained together. Result-type dependencies come from the IR's
// maintained type-use table. Attributes, including predicates and encoding
// parameters, are traversed once during construction. External captures and
// self references in an operation's result types need no scheduling edge.
//
// Nested control other than scf.if/scf.for, or an operation with successors, is
// returned through |out_unstructured_op| for a source-policy diagnostic. The
// body is ready for scheduling when no unsupported operation is returned.
// Status failures identify allocation or size limits. All arrays belong to
// |arena|, borrow the source IR, and remain valid while that IR is unchanged;
// emitting clones does not invalidate them.
iree_status_t loom_scf_body_build(const loom_module_t* module,
                                  const loom_block_t* block,
                                  iree_arena_allocator_t* arena,
                                  loom_scf_body_t* out_body,
                                  const loom_op_t** out_unstructured_op);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_SCF_SCF_BODY_H_
