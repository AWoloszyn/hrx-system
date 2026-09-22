// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <inttypes.h>
#include <string.h>

#include "loom/ir/local_value_domain.h"
#include "loom/ir/module.h"
#include "loom/ops/encoding/ops.h"
#include "loom/ops/encoding/storage.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scf/canonicalize.h"
#include "loom/ops/scf/ops.h"
#include "loom/pass/value_facts.h"
#include "loom/rewrite/rewriter.h"
#include "loom/target/function_version.h"
#include "loom/transforms/encoding/layout_transport.h"
#include "loom/util/walk.h"

#define LOOM_SCF_LAYOUT_TRANSPORT_STATISTICS(V, statistics_type)       \
  V(statistics_type, layouts_decomposed, "layouts-decomposed",         \
    "Number of structured strided-layout results decomposed.")         \
  V(statistics_type, stride_values_inserted, "stride-values-inserted", \
    "Number of index-valued stride results inserted.")                 \
  V(statistics_type, selections_rewritten, "selections-rewritten",     \
    "Number of structured selection operations rewritten.")

LOOM_PASS_STATISTICS_DEFINE(loom_scf_layout_transport_statistics,
                            loom_scf_layout_transport_statistics_t,
                            LOOM_SCF_LAYOUT_TRANSPORT_STATISTICS)

static const loom_pass_info_t kScfLayoutTransportPassInfo = {
    .name = IREE_SVL("decompose-scf-layout-transports"),
    .description =
        IREE_SVL("Carry dynamic strided layouts as scalar SCF payloads."),
    .kind = LOOM_PASS_FUNCTION,
    .statistic_layout = &loom_scf_layout_transport_statistics_layout,
};

const loom_pass_info_t* loom_decompose_scf_layout_transports_pass_info(void) {
  return &kScfLayoutTransportPassInfo;
}

typedef enum loom_scf_layout_projection_kind_e {
  LOOM_SCF_LAYOUT_PROJECTION_VALUE = 0,
  LOOM_SCF_LAYOUT_PROJECTION_CONSTANT = 1,
  LOOM_SCF_LAYOUT_PROJECTION_CANDIDATE_AXIS = 2,
} loom_scf_layout_projection_kind_t;

typedef struct loom_scf_layout_projection_t {
  // How the scalar stride is obtained in the source scope.
  loom_scf_layout_projection_kind_t kind;
  union {
    // Existing index SSA value for VALUE.
    loom_value_id_t value;
    // Exact nonnegative element stride for CONSTANT.
    int64_t constant;
    struct {
      // Candidate whose scalar result supplies the stride.
      iree_host_size_t candidate;
      // Layout axis selecting the candidate's scalar result.
      uint8_t axis;
    } candidate_axis;
  } value;
} loom_scf_layout_projection_t;

typedef struct loom_scf_layout_dependency_t {
  // Candidate requiring the source candidate to be decomposed.
  iree_host_size_t target;
  // Next dependent candidate of the same source.
  iree_host_size_t next;
} loom_scf_layout_dependency_t;

typedef struct loom_scf_layout_candidate_t {
  // Structured selection producing the semantic layout.
  loom_op_t* op;
  // Original layout-valued result.
  loom_value_id_t result;
  // Original role-qualified encoding type.
  loom_type_t type;
  // Static stride or INT64_MIN for each transported dynamic axis.
  int64_t static_strides[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  // Scalar selection result indexed by layout axis; static axes are invalid.
  loom_value_id_t axis_values[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  // Dynamic axes in transported tuple order.
  uint8_t dynamic_axes[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  // Source-major scalar projections for every dynamic axis.
  loom_scf_layout_projection_t* projections;
  // First dependent candidate, or IREE_HOST_SIZE_MAX.
  iree_host_size_t first_dependent;
  // Number of selection sources.
  iree_host_size_t source_count;
  // Result column within the original operation.
  uint16_t result_index;
  // Layout rank.
  uint8_t rank;
  // Number of non-exact axes carried by the selection.
  uint8_t dynamic_count;
  // Whether every source can provide this descriptor.
  bool selected;
} loom_scf_layout_candidate_t;

typedef struct loom_scf_layout_transport_plan_t {
  // Active pass instance for statistics and fact ownership.
  loom_pass_t* pass;
  // Module being rewritten.
  loom_module_t* module;
  // Rewriter maintaining use lists.
  loom_rewriter_t* rewriter;
  // Function-local scratch storage.
  iree_arena_allocator_t* arena;
  // Borrowed function value facts.
  loom_value_fact_table_t* facts;
  // Dense function-local value domain.
  loom_local_value_domain_t domain;
  // Candidate index by local value ordinal, or IREE_HOST_SIZE_MAX.
  iree_host_size_t* candidate_indices;
  // Selection results that may be scalarized, in operation postorder.
  loom_scf_layout_candidate_t* candidates;
  // Number of populated candidates.
  iree_host_size_t candidate_count;
  // Allocated candidate capacity.
  iree_host_size_t candidate_capacity;
  // Reverse dependency edges used to reject incomplete nested selections.
  loom_scf_layout_dependency_t* dependencies;
  // Number of populated dependency edges.
  iree_host_size_t dependency_count;
  // Allocated dependency capacity.
  iree_host_size_t dependency_capacity;
} loom_scf_layout_transport_plan_t;

static bool loom_scf_layout_is_supported_selection(const loom_op_t* op) {
  return loom_scf_if_isa(op) || loom_scf_switch_isa(op) ||
         loom_scf_select_isa(op) || loom_scf_lookup_isa(op);
}

static iree_host_size_t loom_scf_layout_source_count(const loom_op_t* op) {
  if (loom_scf_if_isa(op) || loom_scf_switch_isa(op)) {
    return op->region_count;
  }
  if (loom_scf_select_isa(op)) {
    return 2;
  }
  if (loom_scf_lookup_isa(op)) {
    return (iree_host_size_t)loom_scf_lookup_case_keys(op).count + 1;
  }
  return 0;
}

static bool loom_scf_layout_describe(
    const loom_scf_layout_transport_plan_t* plan, loom_value_id_t value_id,
    loom_scf_layout_candidate_t* out_candidate) {
  const loom_type_t type = loom_module_value_type(plan->module, value_id);
  if (!loom_type_is_encoding(type) ||
      loom_type_encoding_role(type) != LOOM_ENCODING_ROLE_ADDRESS_LAYOUT) {
    return false;
  }

  loom_value_fact_address_layout_t layout = {0};
  if (!loom_encoding_query_value_address_layout(&plan->facts->context, value_id,
                                                &layout) ||
      layout.kind != LOOM_VALUE_FACT_ADDRESS_LAYOUT_STRIDED ||
      layout.rank == 0 || layout.rank > LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK ||
      !layout.strides) {
    return false;
  }

  *out_candidate = (loom_scf_layout_candidate_t){
      .result = value_id,
      .type = type,
      .first_dependent = IREE_HOST_SIZE_MAX,
      .rank = layout.rank,
      .selected = true,
  };
  for (uint8_t axis = 0; axis < layout.rank; ++axis) {
    out_candidate->axis_values[axis] = LOOM_VALUE_ID_INVALID;
    int64_t exact_stride = 0;
    if (loom_value_facts_as_exact_i64(layout.strides[axis], &exact_stride)) {
      if (exact_stride < 0) {
        return false;
      }
      out_candidate->static_strides[axis] = exact_stride;
    } else {
      out_candidate->static_strides[axis] = INT64_MIN;
      out_candidate->dynamic_axes[out_candidate->dynamic_count++] = axis;
    }
  }
  return out_candidate->dynamic_count != 0;
}

static iree_status_t loom_scf_layout_add_candidate(
    loom_scf_layout_transport_plan_t* plan, loom_op_t* op,
    uint16_t result_index) {
  const loom_value_id_t result = loom_op_results(op)[result_index];
  const loom_type_t type = loom_module_value_type(plan->module, result);
  if (!loom_type_is_encoding(type) ||
      loom_type_encoding_role(type) != LOOM_ENCODING_ROLE_ADDRESS_LAYOUT) {
    return iree_ok_status();
  }
  if (plan->candidate_count == plan->candidate_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, plan->candidate_count, plan->candidate_count + 1,
        sizeof(*plan->candidates), &plan->candidate_capacity,
        (void**)&plan->candidates));
  }
  plan->candidates[plan->candidate_count++] = (loom_scf_layout_candidate_t){
      .op = op,
      .result = result,
      .type = type,
      .first_dependent = IREE_HOST_SIZE_MAX,
      .source_count = loom_scf_layout_source_count(op),
      .result_index = result_index,
  };
  return iree_ok_status();
}

static void loom_scf_layout_describe_candidates(
    loom_scf_layout_transport_plan_t* plan) {
  iree_host_size_t described_count = 0;
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    const loom_scf_layout_candidate_t collected = plan->candidates[i];
    loom_scf_layout_candidate_t described;
    if (!loom_scf_layout_describe(plan, collected.result, &described)) {
      continue;
    }
    described.op = collected.op;
    described.result_index = collected.result_index;
    described.source_count = collected.source_count;
    plan->candidates[described_count++] = described;
  }
  plan->candidate_count = described_count;
}

static iree_status_t loom_scf_layout_collect(void* user_data, loom_op_t* op,
                                             const loom_walk_context_t* context,
                                             loom_walk_result_t* out_result) {
  (void)context;
  loom_scf_layout_transport_plan_t* plan = user_data;
  *out_result = LOOM_WALK_CONTINUE;
  if (!loom_scf_layout_is_supported_selection(op)) {
    return iree_ok_status();
  }
  for (uint16_t i = 0; i < op->result_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_scf_layout_add_candidate(plan, op, i));
  }
  return iree_ok_status();
}

static iree_host_size_t loom_scf_layout_candidate_index(
    const loom_scf_layout_transport_plan_t* plan, loom_value_id_t value_id) {
  const loom_value_ordinal_t ordinal =
      loom_local_value_domain_try_ordinal(&plan->domain, value_id);
  return ordinal == LOOM_VALUE_ORDINAL_INVALID
             ? IREE_HOST_SIZE_MAX
             : plan->candidate_indices[ordinal];
}

static iree_status_t loom_scf_layout_index_candidates(
    loom_scf_layout_transport_plan_t* plan) {
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->domain.value_count, sizeof(*plan->candidate_indices),
      (void**)&plan->candidate_indices));
  for (loom_value_ordinal_t i = 0; i < plan->domain.value_count; ++i) {
    plan->candidate_indices[i] = IREE_HOST_SIZE_MAX;
  }
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    const loom_value_ordinal_t ordinal = loom_local_value_domain_ordinal(
        &plan->domain, plan->candidates[i].result);
    plan->candidate_indices[ordinal] = i;
  }
  return iree_ok_status();
}

static bool loom_scf_layout_direct_projection(
    const loom_scf_layout_transport_plan_t* plan, loom_value_id_t source,
    uint8_t rank, uint8_t axis, loom_scf_layout_projection_t* out_projection) {
  loom_value_fact_address_layout_t layout = {0};
  if (!loom_encoding_query_value_address_layout(&plan->facts->context, source,
                                                &layout) ||
      layout.kind != LOOM_VALUE_FACT_ADDRESS_LAYOUT_STRIDED ||
      layout.rank != rank || !layout.strides) {
    return false;
  }
  int64_t exact_stride = 0;
  if (loom_value_facts_as_exact_i64(layout.strides[axis], &exact_stride) &&
      exact_stride >= 0) {
    *out_projection = (loom_scf_layout_projection_t){
        .kind = LOOM_SCF_LAYOUT_PROJECTION_CONSTANT,
        .value.constant = exact_stride,
    };
    return true;
  }
  const loom_value_fact_layout_strides_t bindings =
      loom_encoding_query_value_layout_strides(&plan->facts->context, source);
  if (bindings.count != rank ||
      bindings.values[axis] == LOOM_VALUE_ID_INVALID) {
    return false;
  }
  *out_projection = (loom_scf_layout_projection_t){
      .kind = LOOM_SCF_LAYOUT_PROJECTION_VALUE,
      .value.value = bindings.values[axis],
  };
  return true;
}

static bool loom_scf_layout_projection(
    const loom_scf_layout_transport_plan_t* plan, loom_value_id_t source,
    const loom_scf_layout_candidate_t* target, uint8_t axis,
    loom_scf_layout_projection_t* out_projection,
    iree_host_size_t* out_dependency) {
  *out_dependency = IREE_HOST_SIZE_MAX;
  if (loom_scf_layout_direct_projection(plan, source, target->rank, axis,
                                        out_projection)) {
    return true;
  }

  const iree_host_size_t source_index =
      loom_scf_layout_candidate_index(plan, source);
  if (source_index == IREE_HOST_SIZE_MAX) {
    return false;
  }
  const loom_scf_layout_candidate_t* source_candidate =
      &plan->candidates[source_index];
  if (source_candidate->rank != target->rank ||
      source_candidate->static_strides[axis] != INT64_MIN) {
    return false;
  }
  *out_projection = (loom_scf_layout_projection_t){
      .kind = LOOM_SCF_LAYOUT_PROJECTION_CANDIDATE_AXIS,
      .value.candidate_axis =
          {
              .candidate = source_index,
              .axis = axis,
          },
  };
  *out_dependency = source_index;
  return true;
}

static iree_status_t loom_scf_layout_add_dependency(
    loom_scf_layout_transport_plan_t* plan, iree_host_size_t source,
    iree_host_size_t target) {
  if (source == target) {
    return iree_ok_status();
  }
  if (plan->dependency_count == plan->dependency_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, plan->dependency_count, plan->dependency_count + 1,
        sizeof(*plan->dependencies), &plan->dependency_capacity,
        (void**)&plan->dependencies));
  }
  plan->dependencies[plan->dependency_count] = (loom_scf_layout_dependency_t){
      .target = target,
      .next = plan->candidates[source].first_dependent,
  };
  plan->candidates[source].first_dependent = plan->dependency_count++;
  return iree_ok_status();
}

static bool loom_scf_layout_candidate_source(
    const loom_scf_layout_candidate_t* candidate, iree_host_size_t source_index,
    loom_value_id_t* out_source) {
  loom_op_t* op = candidate->op;
  if (loom_scf_if_isa(op) || loom_scf_switch_isa(op)) {
    loom_op_t* yield =
        loom_scf_region_terminator(loom_op_regions(op)[(uint8_t)source_index]);
    if (!yield || candidate->result_index >= yield->operand_count) {
      return false;
    }
    *out_source = loom_op_const_operands(yield)[candidate->result_index];
    return true;
  }
  if (loom_scf_select_isa(op)) {
    *out_source = source_index == 0 ? loom_scf_select_true_value(op)
                                    : loom_scf_select_false_value(op);
    return true;
  }
  if (loom_scf_lookup_isa(op)) {
    const loom_value_slice_t values = loom_scf_lookup_values(op);
    const iree_host_size_t value_index =
        source_index * op->result_count + candidate->result_index;
    if (value_index >= values.count) {
      return false;
    }
    *out_source = values.values[value_index];
    return true;
  }
  return false;
}

static iree_status_t loom_scf_layout_preflight_candidate(
    loom_scf_layout_transport_plan_t* plan, iree_host_size_t candidate_index) {
  loom_scf_layout_candidate_t* candidate = &plan->candidates[candidate_index];
  const loom_tied_result_t* tied_results = loom_op_tied_results(candidate->op);
  for (uint16_t i = 0; i < candidate->op->tied_result_count; ++i) {
    if (tied_results[i].result_index == candidate->result_index) {
      candidate->selected = false;
    }
  }
  iree_host_size_t projection_count = 0;
  if (!iree_host_size_checked_mul(candidate->source_count,
                                  candidate->dynamic_count,
                                  &projection_count)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "layout projection count overflow");
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, projection_count, sizeof(*candidate->projections),
      (void**)&candidate->projections));

  for (iree_host_size_t source_index = 0;
       source_index < candidate->source_count; ++source_index) {
    loom_value_id_t source = LOOM_VALUE_ID_INVALID;
    if (!loom_scf_layout_candidate_source(candidate, source_index, &source)) {
      candidate->selected = false;
      continue;
    }
    for (uint8_t i = 0; i < candidate->dynamic_count; ++i) {
      const uint8_t axis = candidate->dynamic_axes[i];
      loom_scf_layout_projection_t* projection =
          &candidate->projections[source_index * candidate->dynamic_count + i];
      iree_host_size_t dependency = IREE_HOST_SIZE_MAX;
      if (!loom_scf_layout_projection(plan, source, candidate, axis, projection,
                                      &dependency)) {
        candidate->selected = false;
      } else if (dependency != IREE_HOST_SIZE_MAX) {
        IREE_RETURN_IF_ERROR(
            loom_scf_layout_add_dependency(plan, dependency, candidate_index));
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_scf_layout_propagate_unavailable(
    loom_scf_layout_transport_plan_t* plan) {
  iree_host_size_t* queue = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->candidate_count, sizeof(*queue), (void**)&queue));
  iree_host_size_t head = 0;
  iree_host_size_t tail = 0;
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    if (!plan->candidates[i].selected) {
      queue[tail++] = i;
    }
  }
  while (head < tail) {
    const iree_host_size_t source = queue[head++];
    for (iree_host_size_t edge = plan->candidates[source].first_dependent;
         edge != IREE_HOST_SIZE_MAX; edge = plan->dependencies[edge].next) {
      const iree_host_size_t target = plan->dependencies[edge].target;
      if (plan->candidates[target].selected) {
        plan->candidates[target].selected = false;
        queue[tail++] = target;
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_scf_layout_select_candidates(
    loom_scf_layout_transport_plan_t* plan) {
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_scf_layout_preflight_candidate(plan, i));
  }
  return loom_scf_layout_propagate_unavailable(plan);
}

static iree_status_t loom_scf_layout_name_axis(
    loom_scf_layout_transport_plan_t* plan,
    const loom_scf_layout_candidate_t* candidate, uint8_t axis,
    loom_value_id_t value) {
  char suffix[32] = {0};
  const int length =
      iree_snprintf(suffix, sizeof(suffix), "stride_%" PRIu8, axis);
  if (length <= 0 || (iree_host_size_t)length >= sizeof(suffix)) {
    return iree_ok_status();
  }
  return loom_rewriter_try_set_derived_value_name(
      plan->rewriter, candidate->result, value,
      iree_make_string_view(suffix, (iree_host_size_t)length));
}

static iree_status_t loom_scf_layout_materialize_projection(
    loom_scf_layout_transport_plan_t* plan,
    const loom_scf_layout_projection_t* projection, loom_op_t* anchor,
    loom_value_id_t* out_value) {
  switch (projection->kind) {
    case LOOM_SCF_LAYOUT_PROJECTION_VALUE:
      *out_value = projection->value.value;
      return iree_ok_status();
    case LOOM_SCF_LAYOUT_PROJECTION_CONSTANT: {
      loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
      loom_builder_set_before(&plan->rewriter->builder, anchor);
      loom_op_t* constant_op = NULL;
      iree_status_t status = loom_index_constant_build(
          &plan->rewriter->builder, loom_attr_i64(projection->value.constant),
          loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), anchor->location,
          &constant_op);
      loom_builder_restore(&plan->rewriter->builder, saved_ip);
      IREE_RETURN_IF_ERROR(status);
      *out_value = loom_index_constant_result(constant_op);
      return iree_ok_status();
    }
    case LOOM_SCF_LAYOUT_PROJECTION_CANDIDATE_AXIS: {
      const loom_scf_layout_candidate_t* source =
          &plan->candidates[projection->value.candidate_axis.candidate];
      *out_value = source->axis_values[projection->value.candidate_axis.axis];
      IREE_ASSERT(*out_value != LOOM_VALUE_ID_INVALID);
      return iree_ok_status();
    }
    default:
      IREE_ASSERT(false);
      return iree_ok_status();
  }
}

static iree_status_t loom_scf_layout_materialize_source(
    loom_scf_layout_transport_plan_t* plan,
    const loom_scf_layout_candidate_t* candidate, iree_host_size_t source_index,
    loom_op_t* anchor, loom_value_id_t* out_values) {
  for (uint8_t i = 0; i < candidate->dynamic_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_scf_layout_materialize_projection(
        plan,
        &candidate->projections[source_index * candidate->dynamic_count + i],
        anchor, &out_values[i]));
  }
  return iree_ok_status();
}

static iree_status_t loom_scf_layout_build_reconstruction(
    loom_scf_layout_transport_plan_t* plan,
    loom_scf_layout_candidate_t* candidate, loom_op_t* anchor,
    loom_value_id_t* out_replacement) {
  loom_value_id_t dynamic_strides[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  for (uint8_t i = 0; i < candidate->dynamic_count; ++i) {
    const uint8_t axis = candidate->dynamic_axes[i];
    dynamic_strides[i] = candidate->axis_values[axis];
    IREE_RETURN_IF_ERROR(
        loom_scf_layout_name_axis(plan, candidate, axis, dynamic_strides[i]));
  }
  loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
  loom_builder_set_after(&plan->rewriter->builder, anchor);
  loom_op_t* layout_op = NULL;
  iree_status_t status = loom_encoding_layout_strided_build(
      &plan->rewriter->builder, dynamic_strides, candidate->dynamic_count,
      candidate->static_strides, candidate->rank, candidate->type,
      anchor->location, &layout_op);
  loom_builder_restore(&plan->rewriter->builder, saved_ip);
  IREE_RETURN_IF_ERROR(status);
  *out_replacement = loom_encoding_layout_strided_result(layout_op);
  return iree_ok_status();
}

static iree_status_t loom_scf_layout_copy_comments(loom_module_t* module,
                                                   const loom_op_t* source,
                                                   loom_op_t* target) {
  iree_host_size_t comment_count = 0;
  const iree_string_view_t* comments =
      loom_module_op_comments(module, source, &comment_count);
  return comment_count == 0 ? iree_ok_status()
                            : loom_module_attach_op_comments(
                                  module, target, comments, comment_count);
}

static iree_status_t loom_scf_layout_compute_repacking(
    loom_scf_layout_transport_plan_t* plan, loom_op_t* op,
    uint16_t** out_new_indices, loom_type_t** out_result_types,
    uint16_t* out_result_count) {
  iree_host_size_t final_count = op->result_count;
  for (uint16_t i = 0; i < op->result_count; ++i) {
    const iree_host_size_t candidate_index =
        loom_scf_layout_candidate_index(plan, loom_op_const_results(op)[i]);
    if (candidate_index != IREE_HOST_SIZE_MAX &&
        plan->candidates[candidate_index].selected) {
      final_count += plan->candidates[candidate_index].dynamic_count - 1;
    }
  }
  if (final_count > UINT16_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "structured layout result tuple exceeds %u values",
                            (unsigned)UINT16_MAX);
  }

  uint16_t* new_indices = NULL;
  loom_type_t* result_types = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(plan->arena, op->result_count,
                                                 sizeof(*new_indices),
                                                 (void**)&new_indices));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, final_count, sizeof(*result_types), (void**)&result_types));
  uint16_t next = 0;
  for (uint16_t i = 0; i < op->result_count; ++i) {
    new_indices[i] = next;
    const iree_host_size_t candidate_index =
        loom_scf_layout_candidate_index(plan, loom_op_const_results(op)[i]);
    if (candidate_index == IREE_HOST_SIZE_MAX ||
        !plan->candidates[candidate_index].selected) {
      result_types[next++] =
          loom_module_value_type(plan->module, loom_op_const_results(op)[i]);
      continue;
    }
    const loom_scf_layout_candidate_t* candidate =
        &plan->candidates[candidate_index];
    for (uint8_t j = 0; j < candidate->dynamic_count; ++j) {
      result_types[next++] = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
    }
  }
  IREE_ASSERT_EQ(next, final_count);
  *out_new_indices = new_indices;
  *out_result_types = result_types;
  *out_result_count = (uint16_t)final_count;
  return iree_ok_status();
}

static iree_status_t loom_scf_layout_adjust_tied_results(
    loom_scf_layout_transport_plan_t* plan, const loom_op_t* op,
    const uint16_t* new_indices, uint16_t result_count,
    bool repack_lookup_operands, loom_tied_result_t** out_tied_results) {
  *out_tied_results = NULL;
  if (op->tied_result_count == 0) {
    return iree_ok_status();
  }
  loom_tied_result_t* tied_results = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(plan->arena, op->tied_result_count,
                                sizeof(*tied_results), (void**)&tied_results));
  const loom_tied_result_t* old_tied_results = loom_op_tied_results(op);
  for (uint16_t i = 0; i < op->tied_result_count; ++i) {
    const loom_tied_result_t old_tie = old_tied_results[i];
    IREE_ASSERT_LT(old_tie.result_index, op->result_count);
    tied_results[i] = old_tie;
    tied_results[i].result_index = new_indices[old_tie.result_index];
    if (!repack_lookup_operands || old_tie.operand_index == 0) {
      continue;
    }
    const uint16_t old_value_index = old_tie.operand_index - 1;
    const uint16_t row = old_value_index / op->result_count;
    const uint16_t column = old_value_index % op->result_count;
    const iree_host_size_t new_operand_index =
        1 + (iree_host_size_t)row * result_count + new_indices[column];
    IREE_ASSERT_LE(new_operand_index, UINT16_MAX);
    tied_results[i].operand_index = (uint16_t)new_operand_index;
  }
  *out_tied_results = tied_results;
  return iree_ok_status();
}

static iree_status_t loom_scf_layout_build_region_selection(
    loom_scf_layout_transport_plan_t* plan, loom_op_t* old_op,
    const loom_type_t* result_types, uint16_t result_count,
    const loom_tied_result_t* tied_results, loom_op_t** out_new_op) {
  loom_builder_t* builder = &plan->rewriter->builder;
  if (loom_scf_if_isa(old_op)) {
    const loom_scf_if_build_flags_t flags =
        loom_scf_if_else_region(old_op) ? LOOM_SCF_IF_BUILD_FLAG_HAS_ELSE_REGION
                                        : 0;
    IREE_RETURN_IF_ERROR(loom_scf_if_build(
        builder, flags, loom_scf_if_condition(old_op), result_types,
        result_count, tied_results, old_op->tied_result_count, old_op->location,
        out_new_op));
  } else {
    const loom_attribute_t case_keys = loom_scf_switch_case_keys(old_op);
    IREE_RETURN_IF_ERROR(loom_scf_switch_build(
        builder, loom_scf_switch_selector(old_op), result_types, result_count,
        tied_results, old_op->tied_result_count, case_keys.i64_array,
        case_keys.count, old_op->location, out_new_op));
  }
  (*out_new_op)->instance_flags = old_op->instance_flags;
  return loom_scf_layout_copy_comments(plan->module, old_op, *out_new_op);
}

static iree_status_t loom_scf_layout_rebuild_region_selection(
    loom_scf_layout_transport_plan_t* plan, loom_op_t* old_op) {
  uint16_t* new_indices = NULL;
  loom_type_t* result_types = NULL;
  uint16_t result_count = 0;
  IREE_RETURN_IF_ERROR(loom_scf_layout_compute_repacking(
      plan, old_op, &new_indices, &result_types, &result_count));
  loom_tied_result_t* tied_results = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_layout_adjust_tied_results(
      plan, old_op, new_indices, result_count,
      /*repack_lookup_operands=*/false, &tied_results));

  loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
  loom_builder_set_before(&plan->rewriter->builder, old_op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(plan->rewriter);
  loom_op_t* new_op = NULL;
  iree_status_t status = loom_scf_layout_build_region_selection(
      plan, old_op, result_types, result_count, tied_results, &new_op);
  loom_builder_restore(&plan->rewriter->builder, saved_ip);
  IREE_RETURN_IF_ERROR(status);

  for (uint8_t region_index = 0; region_index < old_op->region_count;
       ++region_index) {
    loom_region_t* old_region = loom_op_regions(old_op)[region_index];
    loom_op_t* old_yield = loom_scf_region_terminator(old_region);
    IREE_ASSERT(old_yield);
    loom_value_id_t* yielded = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, result_count, sizeof(*yielded), (void**)&yielded));
    uint16_t next = 0;
    for (uint16_t i = 0; i < old_op->result_count; ++i) {
      const iree_host_size_t candidate_index = loom_scf_layout_candidate_index(
          plan, loom_op_const_results(old_op)[i]);
      if (candidate_index == IREE_HOST_SIZE_MAX ||
          !plan->candidates[candidate_index].selected) {
        yielded[next++] = loom_op_const_operands(old_yield)[i];
        continue;
      }
      const loom_scf_layout_candidate_t* candidate =
          &plan->candidates[candidate_index];
      IREE_RETURN_IF_ERROR(loom_scf_layout_materialize_source(
          plan, candidate, region_index, old_yield, yielded + next));
      next += candidate->dynamic_count;
    }
    IREE_ASSERT_EQ(next, result_count);

    loom_region_t* new_region = loom_op_regions(new_op)[region_index];
    loom_builder_ip_t region_ip =
        loom_builder_enter_region(&plan->rewriter->builder, new_op, new_region);
    loom_op_t* new_yield = NULL;
    status =
        loom_scf_yield_build(&plan->rewriter->builder, yielded, result_count,
                             old_yield->location, &new_yield);
    loom_builder_restore(&plan->rewriter->builder, region_ip);
    IREE_RETURN_IF_ERROR(status);

    loom_block_t* old_block = loom_region_entry_block(old_region);
    loom_op_t* child_op = old_block->first_op;
    while (child_op && child_op != old_yield) {
      loom_op_t* next_child_op = child_op->next_op;
      IREE_RETURN_IF_ERROR(
          loom_rewriter_move_before(plan->rewriter, child_op, new_yield));
      child_op = next_child_op;
    }
  }

  loom_value_id_t* replacements = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(plan->arena, old_op->result_count,
                                sizeof(*replacements), (void**)&replacements));
  for (uint16_t i = 0; i < old_op->result_count; ++i) {
    const iree_host_size_t candidate_index =
        loom_scf_layout_candidate_index(plan, loom_op_const_results(old_op)[i]);
    if (candidate_index == IREE_HOST_SIZE_MAX ||
        !plan->candidates[candidate_index].selected) {
      replacements[i] = loom_op_results(new_op)[new_indices[i]];
      continue;
    }
    loom_scf_layout_candidate_t* candidate = &plan->candidates[candidate_index];
    for (uint8_t j = 0; j < candidate->dynamic_count; ++j) {
      const uint8_t axis = candidate->dynamic_axes[j];
      candidate->axis_values[axis] =
          loom_op_results(new_op)[new_indices[i] + j];
    }
    IREE_RETURN_IF_ERROR(loom_scf_layout_build_reconstruction(
        plan, candidate, new_op, &replacements[i]));
  }

  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      plan->rewriter, old_op, replacements, old_op->result_count,
      value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(
      plan->rewriter, old_op, replacements, old_op->result_count);
}

static iree_status_t loom_scf_layout_rebuild_select(
    loom_scf_layout_transport_plan_t* plan,
    loom_scf_layout_candidate_t* candidate) {
  loom_op_t* old_op = candidate->op;
  loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
  loom_builder_set_before(&plan->rewriter->builder, old_op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(plan->rewriter);
  loom_value_id_t true_values[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  loom_value_id_t false_values[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  IREE_RETURN_IF_ERROR(loom_scf_layout_materialize_source(plan, candidate, 0,
                                                          old_op, true_values));
  IREE_RETURN_IF_ERROR(loom_scf_layout_materialize_source(
      plan, candidate, 1, old_op, false_values));
  loom_op_t* last_select = NULL;
  for (uint8_t i = 0; i < candidate->dynamic_count; ++i) {
    loom_op_t* scalar_select = NULL;
    IREE_RETURN_IF_ERROR(loom_scf_select_build(
        &plan->rewriter->builder, loom_scf_select_condition(old_op),
        true_values[i], false_values[i],
        loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), old_op->location,
        &scalar_select));
    const uint8_t axis = candidate->dynamic_axes[i];
    candidate->axis_values[axis] = loom_scf_select_result(scalar_select);
    last_select = scalar_select;
  }
  loom_value_id_t replacement = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_scf_layout_build_reconstruction(
      plan, candidate, last_select, &replacement));
  loom_builder_restore(&plan->rewriter->builder, saved_ip);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      plan->rewriter, old_op, &replacement, 1, value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(plan->rewriter, old_op,
                                                  &replacement, 1);
}

static iree_status_t loom_scf_layout_rebuild_lookup(
    loom_scf_layout_transport_plan_t* plan, loom_op_t* old_op) {
  uint16_t* new_indices = NULL;
  loom_type_t* result_types = NULL;
  uint16_t result_count = 0;
  IREE_RETURN_IF_ERROR(loom_scf_layout_compute_repacking(
      plan, old_op, &new_indices, &result_types, &result_count));
  loom_tied_result_t* tied_results = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_layout_adjust_tied_results(
      plan, old_op, new_indices, result_count,
      /*repack_lookup_operands=*/true, &tied_results));
  const loom_attribute_t case_keys = loom_scf_lookup_case_keys(old_op);
  const iree_host_size_t row_count = (iree_host_size_t)case_keys.count + 1;
  iree_host_size_t value_count = 0;
  if (!iree_host_size_checked_mul(row_count, result_count, &value_count)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "layout lookup payload count overflow");
  }
  if (value_count >= UINT16_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "structured layout lookup exceeds %u operands",
                            (unsigned)UINT16_MAX);
  }
  loom_value_id_t* values = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, value_count, sizeof(*values), (void**)&values));
  const loom_value_slice_t old_values = loom_scf_lookup_values(old_op);
  iree_host_size_t next = 0;
  for (iree_host_size_t row = 0; row < row_count; ++row) {
    for (uint16_t i = 0; i < old_op->result_count; ++i) {
      const iree_host_size_t candidate_index = loom_scf_layout_candidate_index(
          plan, loom_op_const_results(old_op)[i]);
      if (candidate_index == IREE_HOST_SIZE_MAX ||
          !plan->candidates[candidate_index].selected) {
        values[next++] = old_values.values[row * old_op->result_count + i];
        continue;
      }
      const loom_scf_layout_candidate_t* candidate =
          &plan->candidates[candidate_index];
      IREE_RETURN_IF_ERROR(loom_scf_layout_materialize_source(
          plan, candidate, row, old_op, values + next));
      next += candidate->dynamic_count;
    }
  }
  IREE_ASSERT_EQ(next, value_count);

  loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
  loom_builder_set_before(&plan->rewriter->builder, old_op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(plan->rewriter);
  loom_op_t* new_op = NULL;
  iree_status_t status = loom_scf_lookup_build(
      &plan->rewriter->builder, loom_scf_lookup_selector(old_op),
      case_keys.i64_array, case_keys.count, values, value_count, result_types,
      result_count, tied_results, old_op->tied_result_count, old_op->location,
      &new_op);
  if (iree_status_is_ok(status)) {
    new_op->instance_flags = old_op->instance_flags;
    status = loom_scf_layout_copy_comments(plan->module, old_op, new_op);
  }
  loom_builder_restore(&plan->rewriter->builder, saved_ip);
  IREE_RETURN_IF_ERROR(status);

  loom_value_id_t* replacements = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(plan->arena, old_op->result_count,
                                sizeof(*replacements), (void**)&replacements));
  for (uint16_t i = 0; i < old_op->result_count; ++i) {
    const iree_host_size_t candidate_index =
        loom_scf_layout_candidate_index(plan, loom_op_const_results(old_op)[i]);
    if (candidate_index == IREE_HOST_SIZE_MAX ||
        !plan->candidates[candidate_index].selected) {
      replacements[i] = loom_op_results(new_op)[new_indices[i]];
      continue;
    }
    loom_scf_layout_candidate_t* candidate = &plan->candidates[candidate_index];
    for (uint8_t j = 0; j < candidate->dynamic_count; ++j) {
      const uint8_t axis = candidate->dynamic_axes[j];
      candidate->axis_values[axis] =
          loom_op_results(new_op)[new_indices[i] + j];
    }
    IREE_RETURN_IF_ERROR(loom_scf_layout_build_reconstruction(
        plan, candidate, new_op, &replacements[i]));
  }
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      plan->rewriter, old_op, replacements, old_op->result_count,
      value_checkpoint));
  return loom_rewriter_replace_all_uses_and_erase(
      plan->rewriter, old_op, replacements, old_op->result_count);
}

static iree_status_t loom_scf_layout_apply(
    loom_scf_layout_transport_plan_t* plan, bool* out_changed) {
  *out_changed = false;
  loom_scf_layout_transport_statistics_t* statistics =
      loom_scf_layout_transport_statistics(plan->pass);
  for (iree_host_size_t i = 0; i < plan->candidate_count;) {
    loom_op_t* op = plan->candidates[i].op;
    iree_host_size_t end = i + 1;
    while (end < plan->candidate_count && plan->candidates[end].op == op) {
      ++end;
    }
    bool selected = false;
    for (iree_host_size_t j = i; j < end; ++j) {
      selected |= plan->candidates[j].selected;
    }
    if (!selected) {
      i = end;
      continue;
    }
    *out_changed = true;
    if (loom_scf_if_isa(op) || loom_scf_switch_isa(op)) {
      IREE_RETURN_IF_ERROR(loom_scf_layout_rebuild_region_selection(plan, op));
    } else if (loom_scf_select_isa(op)) {
      IREE_ASSERT_EQ(end - i, 1u);
      IREE_RETURN_IF_ERROR(
          loom_scf_layout_rebuild_select(plan, &plan->candidates[i]));
    } else {
      IREE_RETURN_IF_ERROR(loom_scf_layout_rebuild_lookup(plan, op));
    }
    ++statistics->selections_rewritten;
    for (iree_host_size_t j = i; j < end; ++j) {
      if (plan->candidates[j].selected) {
        ++statistics->layouts_decomposed;
        statistics->stride_values_inserted += plan->candidates[j].dynamic_count;
      }
    }
    i = end;
  }
  return iree_ok_status();
}

iree_status_t loom_decompose_scf_layout_transports_run(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function) {
  loom_region_t* body = loom_func_like_body(function);
  if (!body) {
    return iree_ok_status();
  }

  loom_rewriter_t rewriter;
  loom_rewriter_initialize(&rewriter, module, pass->arena);
  iree_arena_allocator_t arena;
  iree_arena_initialize(pass->arena->block_pool, &arena);
  loom_scf_layout_transport_plan_t plan = {
      .pass = pass,
      .module = module,
      .rewriter = &rewriter,
      .arena = &arena,
  };

  loom_walk_result_t walk_result;
  iree_status_t status = loom_walk_function(
      module, function, LOOM_WALK_POST_ORDER,
      (loom_walk_callback_t){.fn = loom_scf_layout_collect, .user_data = &plan},
      &arena, &walk_result);
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_local_value_domain_acquire_for_region_tree(
        module, body, &arena, &plan.domain);
  }
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_pass_value_facts_acquire(
        pass, module,
        loom_pass_value_fact_scope_function_for_target(
            function,
            loom_target_function_version_target_facts(pass->function_version)),
        &plan.facts);
  }
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    loom_scf_layout_describe_candidates(&plan);
  }
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_scf_layout_index_candidates(&plan);
  }
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_scf_layout_select_candidates(&plan);
  }
  bool changed = false;
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_scf_layout_apply(&plan, &changed);
  }
  if (changed) {
    loom_pass_value_fact_owner_invalidate(pass->value_facts);
    if (iree_status_is_ok(status)) {
      loom_pass_mark_changed(pass);
    }
  }

  loom_rewriter_deinitialize(&rewriter);
  loom_local_value_domain_release(&plan.domain);
  iree_arena_deinitialize(&arena);
  return status;
}
