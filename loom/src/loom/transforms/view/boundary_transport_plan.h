// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Planned target-selected physical carriers for retained view boundaries.
//
// Planning consumes function-local facts and view-region analyses while their
// scratch leases are active. The retained plan contains only exact carrier
// coordinates and stable IR identities needed by the atomic application phase.

#ifndef LOOM_TRANSFORMS_VIEW_BOUNDARY_TRANSPORT_PLAN_H_
#define LOOM_TRANSFORMS_VIEW_BOUNDARY_TRANSPORT_PLAN_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/analysis/symbolic_expr.h"
#include "loom/analysis/view_regions.h"
#include "loom/ir/local_value_domain.h"
#include "loom/ops/op_defs.h"
#include "loom/pass/types.h"
#include "loom/pass/value_facts.h"
#include "loom/rewrite/rewriter.h"
#include "loom/target/function_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_view_boundary_candidate_kind_e {
  LOOM_VIEW_BOUNDARY_CANDIDATE_BLOCK_ARGUMENT = 0,
  LOOM_VIEW_BOUNDARY_CANDIDATE_CALL_RESULT = 1,
} loom_view_boundary_candidate_kind_t;

typedef struct loom_view_boundary_candidate_t {
  // Semantic view value replaced by this physical carrier.
  loom_value_id_t value_id;
  // Original view type reconstructed at the definition boundary.
  loom_type_t view_type;
  // Block owning a block-argument candidate, otherwise NULL.
  loom_block_t* block;
  // Call owning a result candidate, otherwise NULL.
  loom_op_t* call_op;
  // Future materializing-buffer carrier.
  loom_value_id_t buffer_value_id;
  // Future root-relative byte-offset carrier.
  loom_value_id_t offset_value_id;
  // Reconstructed semantic view, populated while applying the plan.
  loom_value_id_t replacement_value_id;
  // Candidate definition kind.
  loom_view_boundary_candidate_kind_t kind;
} loom_view_boundary_candidate_t;

typedef struct loom_view_boundary_offset_t {
  // View value whose definition anchors offset materialization.
  loom_value_id_t anchor_value_id;
  // Existing or once-materialized complete byte offset.
  loom_value_id_t value_id;
  // Existing scalar or future candidate offset added to expression.
  loom_value_id_t base_value_id;
  // Retained affine expression added to base_value_id.
  const loom_symbolic_expr_t* expression;
  // Candidate supplying base_value_id, or IREE_HOST_SIZE_MAX.
  iree_host_size_t dependency;
} loom_view_boundary_offset_t;

typedef struct loom_view_boundary_coordinate_t {
  // Existing materializing buffer, or INVALID when supplied by a candidate.
  loom_value_id_t buffer_value_id;
  // View-region recipe for the byte offset.
  loom_view_region_id_t region_id;
  // Candidate supplying the root and possibly base offset.
  iree_host_size_t dependency;
  // Correlated root-and-offset selection, or IREE_HOST_SIZE_MAX.
  iree_host_size_t selection;
} loom_view_boundary_coordinate_t;

typedef struct loom_view_boundary_selection_t {
  // Original binary value selection whose alternatives supply the coordinate.
  loom_op_t* op;
  // Selected semantic view result.
  loom_value_id_t result_value_id;
  // Scalar i1 value selecting the true or false coordinate.
  loom_value_id_t condition_value_id;
  // Semantic view selected when condition_value_id is true.
  loom_value_id_t true_value_id;
  // Semantic view selected when condition_value_id is false.
  loom_value_id_t false_value_id;
  // Planned true-value coordinate.
  loom_view_boundary_coordinate_t true_coordinate;
  // Planned false-value coordinate.
  loom_view_boundary_coordinate_t false_coordinate;
  // Materialized selected buffer, or INVALID before application.
  loom_value_id_t buffer_value_id;
  // Materialized selected byte offset, or INVALID before application.
  loom_value_id_t offset_value_id;
  // Whether both alternatives have complete physical coordinates.
  bool selected;
  // Whether recursive planning of this selection is active.
  bool planning;
} loom_view_boundary_selection_t;

typedef struct loom_view_boundary_call_t {
  // Original semantic direct call rebuilt once through CallLike.
  loom_call_like_t call;
  // Callee function plan indexed in the module plan.
  iree_host_size_t callee_index;
  // Coordinates parallel to original call operands; non-view entries unused.
  loom_view_boundary_coordinate_t* operand_coordinates;
  // Preallocated result identities in the rebuilt operation's physical order.
  loom_value_id_t* result_ids;
} loom_view_boundary_call_t;

typedef struct loom_view_boundary_return_t {
  // Original function-body return terminator rebuilt once.
  loom_op_t* op;
  // Coordinates parallel to original return operands; non-view entries unused.
  loom_view_boundary_coordinate_t* coordinates;
} loom_view_boundary_return_t;

typedef struct loom_view_boundary_edge_t {
  // Original single-successor terminator rebuilt once.
  loom_op_t* terminator;
  // Coordinates parallel to the original destination arguments.
  loom_view_boundary_coordinate_t* coordinates;
} loom_view_boundary_edge_t;

typedef struct loom_view_boundary_block_t {
  // Block whose complete signature is rebuilt.
  loom_block_t* block;
  // Original arguments in signature order.
  loom_value_id_t* original_arguments;
  // Number of entries in original_arguments.
  uint16_t original_argument_count;
  // Final physical argument count.
  uint16_t final_argument_count;
  // Incoming single-successor payload edges, empty for a function entry block.
  loom_view_boundary_edge_t* edges;
  // Number of incoming payload edges.
  iree_host_size_t edge_count;
} loom_view_boundary_block_t;

typedef struct loom_view_boundary_function_t {
  // Original function-like operation.
  loom_func_like_t function;
  // Stable compiler version transferred if the function is replaced.
  loom_function_version_t* version;
  // Original logical arguments.
  const loom_value_id_t* arguments;
  // Original argument count.
  uint16_t argument_count;
  // Flat operand offset of declaration arguments, or UINT16_MAX for a body.
  uint16_t argument_operand_offset;
  // Original function result values.
  const loom_value_id_t* results;
  // Original result count.
  uint16_t result_count;
  // Terminator kind returning from the body, or UNKNOWN when bodyless.
  loom_op_kind_t return_kind;
  // Expanded argument count.
  uint16_t final_argument_count;
  // Expanded result count.
  uint16_t final_result_count;
  // Whether each argument is a decomposed view.
  bool* view_arguments;
  // Physical start index for each logical argument.
  uint16_t* argument_indices;
  // Whether each result is a decomposed view.
  bool* view_results;
  // Physical start index for each logical result.
  uint16_t* result_indices;
  // Function-local facts retained until the atomic rewrite completes.
  loom_value_fact_table_t* facts;
  // Function-local correspondence domain.
  loom_local_value_domain_t domain;
  // Symbolic-expression context active while the local domain is acquired.
  loom_symbolic_expr_context_t expressions;
  // View-region analysis over the original function.
  loom_view_region_table_t regions;
  // Offset recipes indexed by view-region ID.
  loom_view_boundary_offset_t* offsets;
  // Correlated selections needed by boundary coordinates.
  loom_view_boundary_selection_t* selections;
  // Number of correlated selections.
  iree_host_size_t selection_count;
  // Allocated correlated-selection capacity.
  iree_host_size_t selection_capacity;
  // Selection index by function-local result ordinal, or IREE_HOST_SIZE_MAX.
  iree_host_size_t* selection_indices;
  // Carrier candidates defined inside this function.
  loom_view_boundary_candidate_t* candidates;
  // Number of carrier candidates.
  iree_host_size_t candidate_count;
  // Allocated candidate capacity.
  iree_host_size_t candidate_capacity;
  // Calls to expanded callees.
  loom_view_boundary_call_t* calls;
  // Number of calls.
  iree_host_size_t call_count;
  // Allocated call capacity.
  iree_host_size_t call_capacity;
  // Returns matching an expanded result signature.
  loom_view_boundary_return_t* returns;
  // Number of returns.
  iree_host_size_t return_count;
  // Allocated return capacity.
  iree_host_size_t return_capacity;
  // Rewritten CFG block signatures.
  loom_view_boundary_block_t* blocks;
  // Number of rewritten blocks.
  iree_host_size_t block_count;
  // Whether this function participates in the rewrite batch.
  bool selected;
  // Whether the signature itself contains a view.
  bool signature_changes;
  // Whether at least one function result expands to a physical pair.
  bool result_signature_changes;
} loom_view_boundary_function_t;

typedef struct loom_view_boundary_plan_t {
  // Active pass instance.
  loom_pass_t* pass;
  // Module being rewritten.
  loom_module_t* module;
  // Pass scratch arena owning the plan.
  iree_arena_allocator_t* arena;
  // Shared rewriter for atomic mutation.
  loom_rewriter_t rewriter;
  // Concrete function-version snapshot by symbol ID.
  loom_target_function_version_snapshot_t versions;
  // Function plans indexed densely.
  loom_view_boundary_function_t* functions;
  // Number of function plans.
  iree_host_size_t function_count;
  // Function plan index by symbol ID.
  iree_host_size_t* function_indices;
  // Number of entries in function_indices.
  iree_host_size_t function_index_count;
  // Function signatures replaced by the application phase.
  int64_t functions_rewritten;
  // Semantic calls replaced by the application phase.
  int64_t calls_rewritten;
  // Function returns replaced by the application phase.
  int64_t returns_rewritten;
  // Direct CFG edges replaced by the application phase.
  int64_t cfg_edges_rewritten;
  // Semantic view values reconstructed from physical carriers.
  int64_t views_decomposed;
} loom_view_boundary_plan_t;

// Builds a complete, non-mutating boundary transport plan.
iree_status_t loom_view_boundary_plan_prepare(
    loom_view_boundary_plan_t* plan,
    const loom_function_version_list_t* version_list);

// Finds a function-local carrier candidate by semantic value identity.
iree_host_size_t loom_view_boundary_candidate_index(
    const loom_view_boundary_function_t* function, loom_value_id_t value_id);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_VIEW_BOUNDARY_TRANSPORT_PLAN_H_
