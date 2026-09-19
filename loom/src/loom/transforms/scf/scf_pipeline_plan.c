// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/scf/scf_pipeline_plan.h"

#include "loom/ir/local_value_domain.h"
#include "loom/ops/op_defs.h"

static iree_status_t loom_scf_pipeline_plan_partition(
    loom_module_t* module, const loom_block_t* block,
    const loom_local_value_domain_t* domain, iree_arena_allocator_t* arena,
    loom_scf_pipeline_plan_t* plan, loom_scf_pipeline_rejection_t* rejection) {
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, plan->body.count, sizeof(*plan->stages), (void**)&plan->stages));
  for (uint32_t i = 0; i < plan->body.count; ++i) {
    const loom_scf_body_operation_t* operation = &plan->body.operations[i];
    plan->stages[i] = LOOM_SCF_PIPELINE_STAGE_CONSUMER;
    if (operation->effects == 0) {
      continue;
    }
    if (operation->effects != LOOM_SCF_BODY_EFFECT_READ) {
      *rejection = (loom_scf_pipeline_rejection_t){
          .op = operation->op,
          .constraint = IREE_SV("ordinary loads and pure operations without "
                                "writes, ordered effects or async groups"),
      };
      return iree_ok_status();
    }
    plan->stages[i] = LOOM_SCF_PIPELINE_STAGE_PRODUCER;
    plan->read_count += operation->load_count;
  }
  if (plan->read_count == 0) {
    *rejection = (loom_scf_pipeline_rejection_t){
        .op = block->first_op->parent_op,
        .constraint = IREE_SV("at least one ordinary load to pipeline"),
    };
    return iree_ok_status();
  }

  uint32_t* producers = NULL;
  bool* queued_values = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, domain->value_count, sizeof(*producers), (void**)&producers));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, domain->value_count,
                                                 sizeof(*queued_values),
                                                 (void**)&queued_values));
  memset(producers, 0xFF, domain->value_count * sizeof(*producers));
  memset(queued_values, 0, domain->value_count * sizeof(*queued_values));
  for (uint32_t i = 0; i < plan->body.count; ++i) {
    const loom_op_t* op = plan->body.operations[i].op;
    const loom_value_id_t* results = loom_op_const_results(op);
    for (uint16_t j = 0; j < op->result_count; ++j) {
      producers[loom_local_value_domain_ordinal(domain, results[j])] = i;
    }
  }

  // Verified SSA puts every captured outer dependency before its scheduling
  // unit. A reverse traversal therefore computes the complete producer cut.
  for (uint32_t reverse = plan->body.count; reverse > 0; --reverse) {
    const uint32_t i = reverse - 1;
    if (plan->stages[i] != LOOM_SCF_PIPELINE_STAGE_PRODUCER) {
      continue;
    }
    const loom_scf_body_operation_t* operation = &plan->body.operations[i];
    for (iree_host_size_t j = 0; j < operation->reference_count; ++j) {
      const loom_scf_body_reference_t* reference =
          &plan->body.references[operation->reference_begin + j];
      if (reference->allow_identity_mapping) {
        continue;
      }
      const loom_value_id_t value_id = reference->value_id;
      if (value_id == block->arg_ids[0]) {
        continue;
      }
      const uint32_t producer =
          producers[loom_local_value_domain_ordinal(domain, value_id)];
      if (producer == UINT32_MAX) {
        *rejection = (loom_scf_pipeline_rejection_t){
            .op = operation->op,
            .constraint = IREE_SV("read-ahead prerequisites independent of "
                                  "loop-carried state"),
        };
        return iree_ok_status();
      }
      plan->stages[producer] = LOOM_SCF_PIPELINE_STAGE_PRODUCER;
    }
  }

  // Include the yield in the consumer: a loaded value may itself be the next
  // carried state without an intervening arithmetic operation.
  for (iree_host_size_t i = 0; i <= plan->body.count; ++i) {
    if (i < plan->body.count &&
        plan->stages[i] == LOOM_SCF_PIPELINE_STAGE_PRODUCER) {
      continue;
    }
    const loom_scf_body_operation_t* operation =
        i == plan->body.count ? &plan->body.terminator
                              : &plan->body.operations[i];
    for (iree_host_size_t j = 0; j < operation->reference_count; ++j) {
      const loom_scf_body_reference_t* reference =
          &plan->body.references[operation->reference_begin + j];
      if (reference->allow_identity_mapping) {
        continue;
      }
      const loom_value_id_t value_id = reference->value_id;
      const loom_value_ordinal_t ordinal =
          loom_local_value_domain_ordinal(domain, value_id);
      const uint32_t producer = producers[ordinal];
      if (value_id == block->arg_ids[0] ||
          (producer != UINT32_MAX &&
           plan->stages[producer] == LOOM_SCF_PIPELINE_STAGE_PRODUCER)) {
        queued_values[ordinal] = true;
      }
    }
  }
  for (loom_value_ordinal_t i = 0; i < domain->value_count; ++i) {
    if (queued_values[i]) {
      ++plan->queue_value_count;
    }
  }
  if (plan->queue_value_count == 0) {
    *rejection = (loom_scf_pipeline_rejection_t){
        .op = block->first_op->parent_op,
        .constraint = IREE_SV("a producer value used by the ordered consumer"),
    };
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, plan->queue_value_count,
                                                 sizeof(*plan->queue_values),
                                                 (void**)&plan->queue_values));
  uint32_t next_queue_value = 0;
  for (loom_value_ordinal_t i = 0; i < domain->value_count; ++i) {
    if (!queued_values[i]) {
      continue;
    }
    const loom_value_id_t value_id = domain->value_ids[i];
    plan->queue_values[next_queue_value++] = value_id;
    loom_type_use_iterator_t dependencies;
    loom_module_value_type_dependencies(module, value_id, &dependencies);
    for (loom_value_id_t provider = loom_type_dependencies_next(&dependencies);
         provider != LOOM_VALUE_ID_INVALID;
         provider = loom_type_dependencies_next(&dependencies)) {
      const loom_value_t* reference = loom_module_value(module, provider);
      const bool local =
          loom_value_is_block_arg(reference)
              ? loom_value_def_block(reference) == block
              : loom_value_def_op(reference)->parent_block == block;
      if (local) {
        *rejection = (loom_scf_pipeline_rejection_t){
            .op = block->first_op->parent_op,
            .constraint = IREE_SV("iteration-invariant types for values "
                                  "carried between pipeline stages"),
        };
        return iree_ok_status();
      }
    }
  }
  return iree_ok_status();
}

iree_status_t loom_scf_pipeline_plan_build(
    loom_module_t* module, const loom_block_t* block,
    iree_arena_allocator_t* arena, loom_scf_pipeline_plan_t* out_plan,
    loom_scf_pipeline_rejection_t* out_rejection) {
  *out_plan = (loom_scf_pipeline_plan_t){0};
  *out_rejection = (loom_scf_pipeline_rejection_t){0};
  const loom_op_t* unstructured_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_body_build(module, block, arena,
                                           &out_plan->body, &unstructured_op));
  if (unstructured_op) {
    *out_rejection = (loom_scf_pipeline_rejection_t){
        .op = unstructured_op,
        .constraint =
            IREE_SV("body operations with only scf.if/scf.for regions "
                    "and no successors"),
    };
    return iree_ok_status();
  }
  loom_local_value_domain_t domain = {0};
  IREE_RETURN_IF_ERROR(loom_local_value_domain_acquire_for_region(
      module, block->parent_region, arena, &domain));
  iree_status_t status = loom_scf_pipeline_plan_partition(
      module, block, &domain, arena, out_plan, out_rejection);
  loom_local_value_domain_release(&domain);
  return status;
}
