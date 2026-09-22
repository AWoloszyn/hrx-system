// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/encoding/layout_transport.h"

#include <inttypes.h>
#include <string.h>

#include "loom/ir/local_value_domain.h"
#include "loom/ir/module.h"
#include "loom/ops/cfg/ops.h"
#include "loom/ops/encoding/ops.h"
#include "loom/ops/encoding/storage.h"
#include "loom/ops/index/ops.h"
#include "loom/pass/value_facts.h"
#include "loom/rewrite/rewriter.h"
#include "loom/target/function_version.h"
#include "loom/util/fact_cfg.h"

#define LOOM_CFG_LAYOUT_TRANSPORT_STATISTICS(V, statistics_type)       \
  V(statistics_type, layouts_decomposed, "layouts-decomposed",         \
    "Number of dynamic strided-layout block arguments decomposed.")    \
  V(statistics_type, stride_args_inserted, "stride-args-inserted",     \
    "Number of index-valued stride block arguments inserted.")         \
  V(statistics_type, branch_edges_rewritten, "branch-edges-rewritten", \
    "Number of CFG branch payloads rewritten for scalar layout transport.")

LOOM_PASS_STATISTICS_DEFINE(loom_cfg_layout_transport_statistics,
                            loom_cfg_layout_transport_statistics_t,
                            LOOM_CFG_LAYOUT_TRANSPORT_STATISTICS)

static const loom_pass_info_t loom_decompose_cfg_layout_transports_info = {
    .name = IREE_SVL("decompose-cfg-layout-transports"),
    .description =
        IREE_SVL("Carry dynamic strided layouts as scalar CFG payloads."),
    .kind = LOOM_PASS_FUNCTION,
    .statistic_layout = &loom_cfg_layout_transport_statistics_layout,
};

const loom_pass_info_t* loom_decompose_cfg_layout_transports_pass_info(void) {
  return &loom_decompose_cfg_layout_transports_info;
}

typedef struct loom_cfg_layout_transport_dependency_t {
  // Candidate requiring the source candidate to be decomposed.
  iree_host_size_t target;
  // Next dependent candidate of the same source.
  iree_host_size_t next;
} loom_cfg_layout_transport_dependency_t;

typedef struct loom_cfg_layout_transport_candidate_t {
  // Block whose signature carries the layout.
  loom_block_t* block;
  // Original layout-valued block argument.
  loom_value_id_t argument;
  // Original role-qualified encoding type.
  loom_type_t type;
  // Static stride or INT64_MIN for each transported dynamic axis.
  int64_t static_strides[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  // Scalar arguments indexed by layout axis; static axes remain INVALID.
  loom_value_id_t stride_args[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  // Reconstructed semantic layout value.
  loom_value_id_t replacement;
  // First dependent candidate, or IREE_HOST_SIZE_MAX.
  iree_host_size_t first_dependent;
  // Layout rank.
  uint8_t rank;
  // Number of non-exact axes carried through the CFG.
  uint8_t dynamic_count;
  // Whether every incoming edge can provide the selected descriptor.
  bool selected;
} loom_cfg_layout_transport_candidate_t;

typedef enum loom_cfg_layout_projection_kind_e {
  LOOM_CFG_LAYOUT_PROJECTION_VALUE = 0,
  LOOM_CFG_LAYOUT_PROJECTION_CONSTANT = 1,
  LOOM_CFG_LAYOUT_PROJECTION_CANDIDATE_AXIS = 2,
} loom_cfg_layout_projection_kind_t;

typedef struct loom_cfg_layout_projection_t {
  // How the scalar stride is obtained at the predecessor edge.
  loom_cfg_layout_projection_kind_t kind;
  union {
    // Existing index SSA value for VALUE.
    loom_value_id_t value;
    // Exact nonnegative element stride for CONSTANT.
    int64_t constant;
    struct {
      // Candidate whose future scalar argument supplies the stride.
      iree_host_size_t candidate;
      // Layout axis selecting the candidate's scalar argument.
      uint8_t axis;
    } candidate_axis;
  } value;
} loom_cfg_layout_projection_t;

typedef struct loom_cfg_layout_transport_edge_t {
  // Original direct branch targeting the rewritten block signature.
  loom_op_t* branch;
  // Scalar projections in selected-candidate and axis order.
  loom_cfg_layout_projection_t* projections;
} loom_cfg_layout_transport_edge_t;

typedef struct loom_cfg_layout_transport_block_t {
  // Block whose selected layout arguments are rewritten together.
  loom_block_t* block;
  // Original argument IDs in signature order.
  loom_value_id_t* original_args;
  // Original argument count.
  uint16_t original_arg_count;
  // Final argument count after descriptor scalarization.
  uint16_t final_arg_count;
  // Incoming direct branches in graph edge order.
  loom_cfg_layout_transport_edge_t* edges;
  // Number of incoming edges.
  iree_host_size_t edge_count;
  // Total scalar projections carried on each incoming edge.
  iree_host_size_t projection_count;
} loom_cfg_layout_transport_block_t;

typedef struct loom_cfg_layout_transport_plan_t {
  // Active pass instance for statistics and fact ownership.
  loom_pass_t* pass;
  // Module being rewritten.
  loom_module_t* module;
  // Rewriter maintaining use lists and value facts.
  loom_rewriter_t* rewriter;
  // Function-local scratch storage.
  iree_arena_allocator_t* arena;
  // Borrowed value facts maintained by the rewriter.
  loom_value_fact_table_t* facts;
  // Borrowed retained CFG snapshot.
  const loom_value_fact_cfg_region_t* cfg;
  // Dense function-local value domain.
  loom_local_value_domain_t domain;
  // Candidate index by local value ordinal, or IREE_HOST_SIZE_MAX.
  iree_host_size_t* candidate_indices;
  // Layout arguments that may be scalarized.
  loom_cfg_layout_transport_candidate_t* candidates;
  // Number of populated candidates.
  iree_host_size_t candidate_count;
  // Allocated candidate capacity.
  iree_host_size_t candidate_capacity;
  // Reverse dependency edges used to reject closed descriptor components.
  loom_cfg_layout_transport_dependency_t* dependencies;
  // Number of populated dependency edges.
  iree_host_size_t dependency_count;
  // Allocated dependency capacity.
  iree_host_size_t dependency_capacity;
  // Rewritten block signatures.
  loom_cfg_layout_transport_block_t* blocks;
  // Number of populated block plans.
  iree_host_size_t block_count;
} loom_cfg_layout_transport_plan_t;

static iree_host_size_t loom_cfg_layout_transport_candidate_index(
    const loom_cfg_layout_transport_plan_t* plan, loom_value_id_t value_id) {
  const loom_value_ordinal_t ordinal =
      loom_local_value_domain_try_ordinal(&plan->domain, value_id);
  return ordinal == LOOM_VALUE_ORDINAL_INVALID
             ? IREE_HOST_SIZE_MAX
             : plan->candidate_indices[ordinal];
}

static bool loom_cfg_layout_transport_has_local_argument_type_use(
    const loom_cfg_layout_transport_plan_t* plan,
    const loom_cfg_layout_transport_candidate_t* candidate) {
  loom_type_use_iterator_t users;
  loom_module_value_type_users(plan->module, candidate->argument, &users);
  for (loom_value_id_t carrier = loom_type_users_next(&users);
       carrier != LOOM_VALUE_ID_INVALID;
       carrier = loom_type_users_next(&users)) {
    const loom_value_t* value = loom_module_value(plan->module, carrier);
    if (loom_value_is_block_arg(value) &&
        loom_value_def_block(value) == candidate->block) {
      return true;
    }
  }
  return false;
}

static bool loom_cfg_layout_transport_describe(
    const loom_cfg_layout_transport_plan_t* plan, loom_value_id_t value_id,
    loom_cfg_layout_transport_candidate_t* out_candidate) {
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

  *out_candidate = (loom_cfg_layout_transport_candidate_t){
      .argument = value_id,
      .type = type,
      .replacement = LOOM_VALUE_ID_INVALID,
      .first_dependent = IREE_HOST_SIZE_MAX,
      .rank = layout.rank,
      .selected = true,
  };
  for (uint8_t axis = 0; axis < layout.rank; ++axis) {
    out_candidate->stride_args[axis] = LOOM_VALUE_ID_INVALID;
    int64_t exact_stride = 0;
    if (loom_value_facts_as_exact_i64(layout.strides[axis], &exact_stride)) {
      if (exact_stride < 0) {
        return false;
      }
      out_candidate->static_strides[axis] = exact_stride;
    } else {
      out_candidate->static_strides[axis] = INT64_MIN;
      ++out_candidate->dynamic_count;
    }
  }
  return out_candidate->dynamic_count != 0;
}

static iree_status_t loom_cfg_layout_transport_add_candidate(
    loom_cfg_layout_transport_plan_t* plan, loom_block_t* block,
    loom_value_id_t argument) {
  loom_cfg_layout_transport_candidate_t candidate;
  if (!loom_cfg_layout_transport_describe(plan, argument, &candidate)) {
    return iree_ok_status();
  }
  if (plan->candidate_count == plan->candidate_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, plan->candidate_count, plan->candidate_count + 1,
        sizeof(*plan->candidates), &plan->candidate_capacity,
        (void**)&plan->candidates));
  }
  candidate.block = block;
  plan->candidates[plan->candidate_count++] = candidate;
  return iree_ok_status();
}

static iree_status_t loom_cfg_layout_transport_collect_candidates(
    loom_cfg_layout_transport_plan_t* plan) {
  const loom_cfg_graph_t* graph = &plan->cfg->graph;
  for (uint16_t block_index = 1; block_index < graph->block_count;
       ++block_index) {
    if (!loom_cfg_graph_block_is_reachable(graph, block_index)) {
      continue;
    }
    loom_block_t* block = (loom_block_t*)graph->blocks[block_index].block;
    for (uint16_t arg_index = 0; arg_index < block->arg_count; ++arg_index) {
      IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_add_candidate(
          plan, block, loom_block_arg_id(block, arg_index)));
    }
  }
  if (plan->candidate_count == 0) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->domain.value_count, sizeof(*plan->candidate_indices),
      (void**)&plan->candidate_indices));
  for (loom_value_ordinal_t i = 0; i < plan->domain.value_count; ++i) {
    plan->candidate_indices[i] = IREE_HOST_SIZE_MAX;
  }
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    const loom_value_ordinal_t ordinal = loom_local_value_domain_ordinal(
        &plan->domain, plan->candidates[i].argument);
    plan->candidate_indices[ordinal] = i;
    if (loom_cfg_layout_transport_has_local_argument_type_use(
            plan, &plan->candidates[i])) {
      plan->candidates[i].selected = false;
    }
  }
  return iree_ok_status();
}

static bool loom_cfg_layout_transport_direct_projection(
    const loom_cfg_layout_transport_plan_t* plan, loom_value_id_t source,
    uint8_t rank, uint8_t axis, loom_cfg_layout_projection_t* out_projection) {
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
    *out_projection = (loom_cfg_layout_projection_t){
        .kind = LOOM_CFG_LAYOUT_PROJECTION_CONSTANT,
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
  *out_projection = (loom_cfg_layout_projection_t){
      .kind = LOOM_CFG_LAYOUT_PROJECTION_VALUE,
      .value.value = bindings.values[axis],
  };
  return true;
}

static bool loom_cfg_layout_transport_projection(
    const loom_cfg_layout_transport_plan_t* plan, loom_value_id_t source,
    const loom_cfg_layout_transport_candidate_t* target, uint8_t axis,
    loom_cfg_layout_projection_t* out_projection,
    iree_host_size_t* out_dependency) {
  *out_dependency = IREE_HOST_SIZE_MAX;
  if (loom_cfg_layout_transport_direct_projection(plan, source, target->rank,
                                                  axis, out_projection)) {
    return true;
  }

  const iree_host_size_t source_index =
      loom_cfg_layout_transport_candidate_index(plan, source);
  if (source_index == IREE_HOST_SIZE_MAX) {
    return false;
  }
  const loom_cfg_layout_transport_candidate_t* source_candidate =
      &plan->candidates[source_index];
  if (source_candidate->rank != target->rank ||
      source_candidate->static_strides[axis] != INT64_MIN) {
    return false;
  }
  *out_projection = (loom_cfg_layout_projection_t){
      .kind = LOOM_CFG_LAYOUT_PROJECTION_CANDIDATE_AXIS,
      .value.candidate_axis =
          {
              .candidate = source_index,
              .axis = axis,
          },
  };
  *out_dependency = source_index;
  return true;
}

static iree_status_t loom_cfg_layout_transport_add_dependency(
    loom_cfg_layout_transport_plan_t* plan, iree_host_size_t source,
    iree_host_size_t target) {
  if (plan->dependency_count == plan->dependency_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, plan->dependency_count, plan->dependency_count + 1,
        sizeof(*plan->dependencies), &plan->dependency_capacity,
        (void**)&plan->dependencies));
  }
  plan->dependencies[plan->dependency_count] =
      (loom_cfg_layout_transport_dependency_t){
          .target = target,
          .next = plan->candidates[source].first_dependent,
      };
  plan->candidates[source].first_dependent = plan->dependency_count++;
  return iree_ok_status();
}

static iree_status_t loom_cfg_layout_transport_preflight_candidate(
    loom_cfg_layout_transport_plan_t* plan, iree_host_size_t candidate_index) {
  loom_cfg_layout_transport_candidate_t* candidate =
      &plan->candidates[candidate_index];
  const loom_cfg_graph_t* graph = &plan->cfg->graph;
  const uint16_t block_index = candidate->block->region_index;
  const loom_cfg_edge_index_span_t edges =
      loom_cfg_graph_predecessor_edges(graph, block_index);
  const uint16_t arg_index = loom_value_def_index(
      loom_module_value(plan->module, candidate->argument));
  for (iree_host_size_t i = 0; i < edges.count; ++i) {
    const loom_cfg_edge_info_t* edge =
        loom_cfg_graph_edge(graph, edges.values[i]);
    loom_op_t* branch = edge ? (loom_op_t*)edge->terminator : NULL;
    if (!branch || !loom_cfg_br_isa(branch) ||
        loom_cfg_br_dest(branch) != candidate->block) {
      candidate->selected = false;
      continue;
    }
    const loom_value_slice_t args = loom_cfg_br_args(branch);
    if (args.count != candidate->block->arg_count) {
      candidate->selected = false;
      continue;
    }
    const loom_value_id_t source = args.values[arg_index];
    for (uint8_t axis = 0; axis < candidate->rank; ++axis) {
      if (candidate->static_strides[axis] != INT64_MIN) {
        continue;
      }
      loom_cfg_layout_projection_t projection;
      iree_host_size_t dependency = IREE_HOST_SIZE_MAX;
      if (!loom_cfg_layout_transport_projection(plan, source, candidate, axis,
                                                &projection, &dependency)) {
        candidate->selected = false;
      } else if (dependency != IREE_HOST_SIZE_MAX) {
        IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_add_dependency(
            plan, dependency, candidate_index));
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_cfg_layout_transport_propagate_unavailable(
    loom_cfg_layout_transport_plan_t* plan) {
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

static iree_status_t loom_cfg_layout_transport_select_candidates(
    loom_cfg_layout_transport_plan_t* plan) {
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    IREE_RETURN_IF_ERROR(
        loom_cfg_layout_transport_preflight_candidate(plan, i));
  }
  IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_propagate_unavailable(plan));

  bool rejected_for_capacity = false;
  const loom_cfg_graph_t* graph = &plan->cfg->graph;
  for (uint16_t block_index = 1; block_index < graph->block_count;
       ++block_index) {
    const loom_block_t* block = graph->blocks[block_index].block;
    iree_host_size_t final_count = block->arg_count;
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      const iree_host_size_t candidate_index =
          loom_cfg_layout_transport_candidate_index(
              plan, loom_block_arg_id(block, i));
      if (candidate_index == IREE_HOST_SIZE_MAX ||
          !plan->candidates[candidate_index].selected) {
        continue;
      }
      final_count += plan->candidates[candidate_index].dynamic_count - 1;
    }
    if (final_count <= UINT16_MAX) {
      continue;
    }
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      const iree_host_size_t candidate_index =
          loom_cfg_layout_transport_candidate_index(
              plan, loom_block_arg_id(block, i));
      if (candidate_index != IREE_HOST_SIZE_MAX &&
          plan->candidates[candidate_index].selected) {
        plan->candidates[candidate_index].selected = false;
        rejected_for_capacity = true;
      }
    }
  }
  return rejected_for_capacity
             ? loom_cfg_layout_transport_propagate_unavailable(plan)
             : iree_ok_status();
}

static iree_status_t loom_cfg_layout_transport_build_block_plans(
    loom_cfg_layout_transport_plan_t* plan) {
  const loom_cfg_graph_t* graph = &plan->cfg->graph;
  iree_host_size_t selected_block_count = 0;
  for (uint16_t block_index = 1; block_index < graph->block_count;
       ++block_index) {
    const loom_block_t* block = graph->blocks[block_index].block;
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      const iree_host_size_t candidate_index =
          loom_cfg_layout_transport_candidate_index(
              plan, loom_block_arg_id(block, i));
      if (candidate_index != IREE_HOST_SIZE_MAX &&
          plan->candidates[candidate_index].selected) {
        ++selected_block_count;
        break;
      }
    }
  }
  if (selected_block_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(plan->arena, selected_block_count,
                                sizeof(*plan->blocks), (void**)&plan->blocks));

  for (uint16_t block_index = 1; block_index < graph->block_count;
       ++block_index) {
    loom_block_t* block = (loom_block_t*)graph->blocks[block_index].block;
    iree_host_size_t projection_count = 0;
    iree_host_size_t final_count = block->arg_count;
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      const iree_host_size_t candidate_index =
          loom_cfg_layout_transport_candidate_index(
              plan, loom_block_arg_id(block, i));
      if (candidate_index == IREE_HOST_SIZE_MAX ||
          !plan->candidates[candidate_index].selected) {
        continue;
      }
      const uint8_t dynamic_count =
          plan->candidates[candidate_index].dynamic_count;
      projection_count += dynamic_count;
      final_count += dynamic_count - 1;
    }
    if (projection_count == 0) {
      continue;
    }

    loom_cfg_layout_transport_block_t* block_plan =
        &plan->blocks[plan->block_count++];
    *block_plan = (loom_cfg_layout_transport_block_t){
        .block = block,
        .original_arg_count = block->arg_count,
        .final_arg_count = (uint16_t)final_count,
        .projection_count = projection_count,
    };
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, block->arg_count, sizeof(*block_plan->original_args),
        (void**)&block_plan->original_args));
    memcpy(block_plan->original_args, block->arg_ids,
           block->arg_count * sizeof(*block->arg_ids));

    const loom_cfg_edge_index_span_t edge_indices =
        loom_cfg_graph_predecessor_edges(graph, block_index);
    block_plan->edge_count = edge_indices.count;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, edge_indices.count, sizeof(*block_plan->edges),
        (void**)&block_plan->edges));
    for (iree_host_size_t edge_ordinal = 0; edge_ordinal < edge_indices.count;
         ++edge_ordinal) {
      const loom_cfg_edge_info_t* edge =
          loom_cfg_graph_edge(graph, edge_indices.values[edge_ordinal]);
      loom_cfg_layout_transport_edge_t* edge_plan =
          &block_plan->edges[edge_ordinal];
      edge_plan->branch = (loom_op_t*)edge->terminator;
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          plan->arena, projection_count, sizeof(*edge_plan->projections),
          (void**)&edge_plan->projections));
      const loom_value_slice_t sources = loom_cfg_br_args(edge_plan->branch);
      iree_host_size_t projection_ordinal = 0;
      for (uint16_t arg_index = 0; arg_index < block->arg_count; ++arg_index) {
        const iree_host_size_t candidate_index =
            loom_cfg_layout_transport_candidate_index(
                plan, block_plan->original_args[arg_index]);
        if (candidate_index == IREE_HOST_SIZE_MAX ||
            !plan->candidates[candidate_index].selected) {
          continue;
        }
        const loom_cfg_layout_transport_candidate_t* candidate =
            &plan->candidates[candidate_index];
        for (uint8_t axis = 0; axis < candidate->rank; ++axis) {
          if (candidate->static_strides[axis] != INT64_MIN) {
            continue;
          }
          iree_host_size_t dependency = IREE_HOST_SIZE_MAX;
          const bool available = loom_cfg_layout_transport_projection(
              plan, sources.values[arg_index], candidate, axis,
              &edge_plan->projections[projection_ordinal++], &dependency);
          IREE_ASSERT(available);
          IREE_ASSERT(dependency == IREE_HOST_SIZE_MAX ||
                      plan->candidates[dependency].selected);
        }
      }
      IREE_ASSERT_EQ(projection_ordinal, projection_count);
    }
  }
  IREE_ASSERT_EQ(plan->block_count, selected_block_count);
  return iree_ok_status();
}

static iree_status_t loom_cfg_layout_transport_name_stride(
    loom_cfg_layout_transport_plan_t* plan,
    const loom_cfg_layout_transport_candidate_t* candidate, uint8_t axis,
    loom_value_id_t stride_arg) {
  char suffix[32] = {0};
  const int length =
      iree_snprintf(suffix, sizeof(suffix), "stride_%" PRIu8, axis);
  if (length <= 0 || (iree_host_size_t)length >= sizeof(suffix)) {
    return iree_ok_status();
  }
  return loom_rewriter_try_set_derived_value_name(
      plan->rewriter, candidate->argument, stride_arg,
      iree_make_string_view(suffix, (iree_host_size_t)length));
}

static iree_status_t loom_cfg_layout_transport_build_reconstruction(
    loom_cfg_layout_transport_plan_t* plan,
    loom_cfg_layout_transport_candidate_t* candidate) {
  loom_value_id_t dynamic_strides[LOOM_ENCODING_ADDRESS_LAYOUT_MAX_RANK];
  iree_host_size_t dynamic_count = 0;
  const uint16_t original_index = loom_value_def_index(
      loom_module_value(plan->module, candidate->argument));
  for (uint8_t axis = 0; axis < candidate->rank; ++axis) {
    if (candidate->static_strides[axis] != INT64_MIN) {
      continue;
    }
    loom_value_id_t stride_arg = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_module_define_value(
        plan->module, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), &stride_arg));
    IREE_RETURN_IF_ERROR(loom_block_insert_arg(
        plan->module, candidate->block,
        (uint16_t)(original_index + dynamic_count), stride_arg));
    candidate->stride_args[axis] = stride_arg;
    dynamic_strides[dynamic_count++] = stride_arg;
    IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_name_stride(
        plan, candidate, axis, stride_arg));
  }
  IREE_ASSERT_EQ(dynamic_count, candidate->dynamic_count);

  loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
  loom_builder_set_before(&plan->rewriter->builder, candidate->block->first_op);
  loom_op_t* layout_op = NULL;
  iree_status_t status = loom_encoding_layout_strided_build(
      &plan->rewriter->builder, dynamic_strides, dynamic_count,
      candidate->static_strides, candidate->rank, candidate->type,
      candidate->block->first_op->location, &layout_op);
  loom_builder_restore(&plan->rewriter->builder, saved_ip);
  IREE_RETURN_IF_ERROR(status);
  candidate->replacement = loom_encoding_layout_strided_result(layout_op);
  return loom_rewriter_move_value_name(plan->rewriter, candidate->argument,
                                       candidate->replacement);
}

static iree_status_t loom_cfg_layout_transport_materialize_projection(
    loom_cfg_layout_transport_plan_t* plan,
    const loom_cfg_layout_projection_t* projection, loom_op_t* branch,
    loom_value_id_t* out_value) {
  switch (projection->kind) {
    case LOOM_CFG_LAYOUT_PROJECTION_VALUE:
      *out_value = projection->value.value;
      return iree_ok_status();
    case LOOM_CFG_LAYOUT_PROJECTION_CANDIDATE_AXIS: {
      const loom_cfg_layout_transport_candidate_t* candidate =
          &plan->candidates[projection->value.candidate_axis.candidate];
      *out_value =
          candidate->stride_args[projection->value.candidate_axis.axis];
      IREE_ASSERT(*out_value != LOOM_VALUE_ID_INVALID);
      return iree_ok_status();
    }
    case LOOM_CFG_LAYOUT_PROJECTION_CONSTANT: {
      loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
      loom_builder_set_before(&plan->rewriter->builder, branch);
      loom_op_t* constant_op = NULL;
      iree_status_t status = loom_index_constant_build(
          &plan->rewriter->builder, loom_attr_i64(projection->value.constant),
          loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), branch->location,
          &constant_op);
      loom_builder_restore(&plan->rewriter->builder, saved_ip);
      IREE_RETURN_IF_ERROR(status);
      *out_value = loom_index_constant_result(constant_op);
      return iree_ok_status();
    }
    default:
      IREE_ASSERT(false);
      return iree_ok_status();
  }
}

static iree_status_t loom_cfg_layout_transport_rebuild_edge(
    loom_cfg_layout_transport_plan_t* plan,
    const loom_cfg_layout_transport_block_t* block_plan,
    const loom_cfg_layout_transport_edge_t* edge_plan) {
  loom_op_t* old_branch = edge_plan->branch;
  const loom_value_slice_t old_args = loom_cfg_br_args(old_branch);
  loom_value_id_t* new_args = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(plan->arena, block_plan->final_arg_count,
                                sizeof(*new_args), (void**)&new_args));

  iree_host_size_t new_ordinal = 0;
  iree_host_size_t projection_ordinal = 0;
  for (uint16_t arg_index = 0; arg_index < block_plan->original_arg_count;
       ++arg_index) {
    const iree_host_size_t candidate_index =
        loom_cfg_layout_transport_candidate_index(
            plan, block_plan->original_args[arg_index]);
    if (candidate_index == IREE_HOST_SIZE_MAX ||
        !plan->candidates[candidate_index].selected) {
      new_args[new_ordinal++] = old_args.values[arg_index];
      continue;
    }
    const loom_cfg_layout_transport_candidate_t* candidate =
        &plan->candidates[candidate_index];
    for (uint8_t axis = 0; axis < candidate->rank; ++axis) {
      if (candidate->static_strides[axis] != INT64_MIN) {
        continue;
      }
      IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_materialize_projection(
          plan, &edge_plan->projections[projection_ordinal++], old_branch,
          &new_args[new_ordinal++]));
    }
  }
  IREE_ASSERT_EQ(new_ordinal, block_plan->final_arg_count);
  IREE_ASSERT_EQ(projection_ordinal, block_plan->projection_count);

  loom_builder_ip_t saved_ip = loom_builder_save(&plan->rewriter->builder);
  loom_builder_set_before(&plan->rewriter->builder, old_branch);
  loom_op_t* new_branch = NULL;
  iree_status_t status =
      loom_cfg_br_build(&plan->rewriter->builder, block_plan->block, new_args,
                        new_ordinal, old_branch->location, &new_branch);
  loom_builder_restore(&plan->rewriter->builder, saved_ip);
  IREE_RETURN_IF_ERROR(status);
  return loom_rewriter_erase(plan->rewriter, old_branch);
}

static iree_status_t loom_cfg_layout_transport_apply(
    loom_cfg_layout_transport_plan_t* plan, bool* out_changed) {
  *out_changed = false;
  if (plan->block_count == 0) {
    return iree_ok_status();
  }
  // Every operation below is part of one preflighted batch. Record that the
  // active fact scope may need invalidation if an allocation failure interrupts
  // the batch after its first mutation.
  *out_changed = true;

  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    if (plan->candidates[i].selected) {
      IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_build_reconstruction(
          plan, &plan->candidates[i]));
    }
  }
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    loom_cfg_layout_transport_candidate_t* candidate = &plan->candidates[i];
    if (!candidate->selected) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
        plan->rewriter, candidate->argument, candidate->replacement));
  }
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    loom_cfg_layout_transport_candidate_t* candidate = &plan->candidates[i];
    if (!candidate->selected) {
      continue;
    }
    const uint16_t arg_index = loom_value_def_index(
        loom_module_value(plan->module, candidate->argument));
    IREE_RETURN_IF_ERROR(
        loom_block_remove_arg(plan->module, candidate->block, arg_index));
  }

  loom_cfg_layout_transport_statistics_t* statistics =
      loom_cfg_layout_transport_statistics(plan->pass);
  for (iree_host_size_t i = 0; i < plan->block_count; ++i) {
    const loom_cfg_layout_transport_block_t* block_plan = &plan->blocks[i];
    for (iree_host_size_t edge = 0; edge < block_plan->edge_count; ++edge) {
      IREE_RETURN_IF_ERROR(loom_cfg_layout_transport_rebuild_edge(
          plan, block_plan, &block_plan->edges[edge]));
      ++statistics->branch_edges_rewritten;
    }
  }
  for (iree_host_size_t i = 0; i < plan->candidate_count; ++i) {
    if (!plan->candidates[i].selected) {
      continue;
    }
    ++statistics->layouts_decomposed;
    statistics->stride_args_inserted += plan->candidates[i].dynamic_count;
  }

  IREE_RETURN_IF_ERROR(loom_rewriter_refresh_cfg_facts(
      plan->rewriter, (loom_region_t*)plan->cfg->graph.region));
  loom_op_t* pending_op = NULL;
  while ((pending_op = loom_rewriter_pop(plan->rewriter)) != NULL) {
    bool folded = false;
    IREE_RETURN_IF_ERROR(
        loom_rewriter_try_fold(plan->rewriter, pending_op, &folded));
  }
  return iree_ok_status();
}

static bool loom_cfg_layout_transport_may_have_candidate(
    const loom_module_t* module, const loom_region_t* body) {
  for (uint16_t block_index = 1; block_index < body->block_count;
       ++block_index) {
    const loom_block_t* block = loom_region_const_block(body, block_index);
    for (uint16_t arg_index = 0; arg_index < block->arg_count; ++arg_index) {
      const loom_type_t type =
          loom_module_value_type(module, loom_block_arg_id(block, arg_index));
      if (loom_type_is_encoding(type) &&
          loom_type_encoding_role(type) == LOOM_ENCODING_ROLE_ADDRESS_LAYOUT) {
        return true;
      }
    }
  }
  return false;
}

iree_status_t loom_decompose_cfg_layout_transports_run(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function) {
  loom_region_t* body = loom_func_like_body(function);
  if (!body || body->block_count < 2 ||
      !loom_cfg_layout_transport_may_have_candidate(module, body)) {
    return iree_ok_status();
  }

  loom_rewriter_t rewriter;
  loom_rewriter_initialize(&rewriter, module, pass->arena);
  iree_arena_allocator_t arena;
  iree_arena_initialize(pass->arena->block_pool, &arena);
  loom_cfg_layout_transport_plan_t plan = {
      .pass = pass,
      .module = module,
      .rewriter = &rewriter,
      .arena = &arena,
  };

  iree_status_t status = loom_local_value_domain_acquire_for_region_tree(
      module, body, &arena, &plan.domain);
  if (iree_status_is_ok(status)) {
    status = loom_pass_value_facts_acquire(
        pass, module,
        loom_pass_value_fact_scope_function_for_target(
            function,
            loom_target_function_version_target_facts(pass->function_version)),
        &plan.facts);
  }
  if (iree_status_is_ok(status)) {
    plan.cfg = loom_value_fact_table_lookup_cfg_region(plan.facts, body);
  }
  if (iree_status_is_ok(status) && plan.cfg && !plan.cfg->graph.malformed) {
    status = loom_rewriter_enable_worklist(&rewriter);
  }
  if (iree_status_is_ok(status) && plan.cfg && !plan.cfg->graph.malformed) {
    loom_rewriter_attach_value_facts(&rewriter, plan.facts);
    status = loom_cfg_layout_transport_collect_candidates(&plan);
  }
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_cfg_layout_transport_select_candidates(&plan);
  }
  if (iree_status_is_ok(status) && plan.candidate_count != 0) {
    status = loom_cfg_layout_transport_build_block_plans(&plan);
  }
  bool changed = false;
  if (iree_status_is_ok(status) && plan.block_count != 0) {
    status = loom_cfg_layout_transport_apply(&plan, &changed);
  }
  if (iree_status_is_ok(status) && changed) {
    loom_pass_mark_changed(pass);
  } else if (!iree_status_is_ok(status)) {
    loom_pass_value_fact_owner_invalidate(pass->value_facts);
  }

  loom_rewriter_deinitialize(&rewriter);
  loom_local_value_domain_release(&plan.domain);
  iree_arena_deinitialize(&arena);
  return status;
}
