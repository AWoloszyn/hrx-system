// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/view/boundary_transport_plan.h"

#include <stdlib.h>
#include <string.h>

#include "loom/ir/module.h"
#include "loom/target/pass_environment.h"
#include "loom/target/provider.h"
#include "loom/util/fact_cfg.h"
#include "loom/util/walk.h"

static bool loom_view_boundary_provider_uses_buffer_offset(
    const loom_target_function_version_t* version) {
  return version != NULL && version->resolved_target.provider != NULL &&
         version->resolved_target.provider->view_boundary_carrier ==
             LOOM_TARGET_VIEW_BOUNDARY_CARRIER_BUFFER_OFFSET;
}

static bool loom_view_boundary_value_has_nonoperand_uses(
    const loom_module_t* module, loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(module, value_id);
  return loom_module_value_has_type_uses(module, value_id) ||
         loom_value_has_attribute_uses(value);
}

static bool loom_view_boundary_type_requires_coupled_transport(
    loom_type_t type) {
  // An SSA encoding is another physical boundary value whose decomposition
  // must stay coordinated with the view. This pass owns only the view pair;
  // selecting either half alone would discard the runtime layout contract.
  return loom_type_has_ssa_encoding(type);
}

static bool loom_view_boundary_tie_uses_expanded_slot(
    const loom_view_boundary_function_t* function,
    const loom_tied_result_t* tie) {
  if (function->argument_operand_offset != UINT16_MAX &&
      tie->operand_index >= function->argument_operand_offset) {
    const uint16_t argument_index =
        (uint16_t)(tie->operand_index - function->argument_operand_offset);
    if (argument_index < function->argument_count &&
        function->view_arguments[argument_index]) {
      return true;
    }
  }
  return tie->result_index < function->result_count &&
         function->view_results[tie->result_index];
}

static bool loom_view_boundary_call_tie_uses_expanded_slot(
    loom_call_like_t call, const loom_view_boundary_function_t* callee,
    const loom_tied_result_t* tie) {
  const uint16_t operand_offset = loom_call_like_operand_offset(call);
  const uint16_t result_offset = loom_call_like_result_offset(call);
  if (tie->operand_index >= operand_offset) {
    const uint16_t argument_index =
        (uint16_t)(tie->operand_index - operand_offset);
    if (argument_index < callee->argument_count &&
        callee->view_arguments[argument_index]) {
      return true;
    }
  }
  if (tie->result_index >= result_offset) {
    const uint16_t result_index = (uint16_t)(tie->result_index - result_offset);
    if (result_index < callee->result_count &&
        callee->view_results[result_index]) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_view_boundary_plan_function_signature(
    loom_view_boundary_plan_t* plan, loom_func_like_t function,
    loom_function_version_t* version,
    loom_view_boundary_function_t* out_function) {
  memset(out_function, 0, sizeof(*out_function));
  out_function->function = function;
  out_function->version = version;
  out_function->argument_operand_offset = UINT16_MAX;
  const loom_value_id_t* arguments =
      loom_func_like_arg_ids(function, &out_function->argument_count);
  if (out_function->argument_count != 0) {
    loom_value_id_t* argument_copy = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, out_function->argument_count, sizeof(*argument_copy),
        (void**)&argument_copy));
    memcpy(argument_copy, arguments,
           out_function->argument_count * sizeof(*argument_copy));
    out_function->arguments = argument_copy;
  }
  out_function->result_count = function.op->result_count;
  if (out_function->result_count != 0) {
    loom_value_id_t* result_copy = NULL;
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(plan->arena, out_function->result_count,
                                  sizeof(*result_copy), (void**)&result_copy));
    memcpy(result_copy, loom_op_const_results(function.op),
           out_function->result_count * sizeof(*result_copy));
    out_function->results = result_copy;
  }
  out_function->selected = true;

  if (!loom_func_like_isa(function) || function.op->successor_count != 0) {
    out_function->selected = false;
    return iree_ok_status();
  }
  loom_region_t* body = loom_func_like_body(function);
  if (body) {
    const uint8_t body_region_index =
        loom_func_like_body_region_index(function);
    const loom_op_vtable_t* op_vtable =
        loom_op_vtable(plan->module, function.op);
    const loom_region_descriptor_t* body_descriptor =
        loom_op_vtable_region_descriptor(op_vtable, body_region_index);
    if (!body_descriptor ||
        body_descriptor->terminator == LOOM_OP_KIND_UNKNOWN ||
        function.op->region_count != 1 || body_region_index != 0) {
      out_function->selected = false;
      return iree_ok_status();
    }
    out_function->return_kind = body_descriptor->terminator;
  } else {
    if (function.op->region_count != 0 ||
        (function.vtable->args_operand_field_index == LOOM_OPERAND_INDEX_NONE &&
         function.op->operand_count != 0)) {
      out_function->selected = false;
      return iree_ok_status();
    }
    if (function.vtable->args_operand_field_index != LOOM_OPERAND_INDEX_NONE) {
      const loom_op_vtable_t* op_vtable =
          loom_op_vtable(plan->module, function.op);
      const loom_value_slice_t argument_span = loom_op_operand_field_span(
          op_vtable, function.op, function.vtable->args_operand_field_index);
      IREE_ASSERT_EQ(argument_span.count, out_function->argument_count);
      out_function->argument_operand_offset =
          (uint16_t)(argument_span.values -
                     loom_op_const_operands(function.op));
    }
    out_function->return_kind = LOOM_OP_KIND_UNKNOWN;
  }

  if (out_function->argument_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(plan->arena, out_function->argument_count,
                                  sizeof(*out_function->view_arguments),
                                  (void**)&out_function->view_arguments));
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(plan->arena, out_function->argument_count,
                                  sizeof(*out_function->argument_indices),
                                  (void**)&out_function->argument_indices));
    memset(
        out_function->view_arguments, 0,
        out_function->argument_count * sizeof(*out_function->view_arguments));
  }
  uint32_t final_argument_count = 0;
  for (uint16_t i = 0; i < out_function->argument_count; ++i) {
    out_function->argument_indices[i] =
        final_argument_count <= UINT16_MAX ? (uint16_t)final_argument_count : 0;
    const loom_type_t type =
        loom_module_value_type(plan->module, out_function->arguments[i]);
    if (!loom_type_is_view(type)) {
      ++final_argument_count;
      continue;
    }
    out_function->view_arguments[i] = true;
    out_function->signature_changes = true;
    final_argument_count += 2;
    if (loom_view_boundary_type_requires_coupled_transport(type) ||
        loom_view_boundary_value_has_nonoperand_uses(
            plan->module, out_function->arguments[i])) {
      out_function->selected = false;
    }
  }
  if (final_argument_count > UINT16_MAX) {
    out_function->selected = false;
  } else {
    out_function->final_argument_count = (uint16_t)final_argument_count;
  }

  if (out_function->result_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(plan->arena, out_function->result_count,
                                  sizeof(*out_function->view_results),
                                  (void**)&out_function->view_results));
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(plan->arena, out_function->result_count,
                                  sizeof(*out_function->result_indices),
                                  (void**)&out_function->result_indices));
    memset(out_function->view_results, 0,
           out_function->result_count * sizeof(*out_function->view_results));
  }
  uint32_t final_result_count = 0;
  for (uint16_t i = 0; i < out_function->result_count; ++i) {
    out_function->result_indices[i] =
        final_result_count <= UINT16_MAX ? (uint16_t)final_result_count : 0;
    const loom_type_t type =
        loom_module_value_type(plan->module, out_function->results[i]);
    if (!loom_type_is_view(type)) {
      ++final_result_count;
      continue;
    }
    out_function->view_results[i] = true;
    out_function->signature_changes = true;
    out_function->result_signature_changes = true;
    final_result_count += 2;
    if (loom_view_boundary_type_requires_coupled_transport(type) ||
        loom_view_boundary_value_has_nonoperand_uses(
            plan->module, out_function->results[i]) ||
        loom_module_value(plan->module, out_function->results[i])->use_count !=
            0) {
      out_function->selected = false;
    }
  }
  if (final_result_count > UINT16_MAX) {
    out_function->selected = false;
  } else {
    out_function->final_result_count = (uint16_t)final_result_count;
  }

  const loom_tied_result_t* ties = loom_op_tied_results(function.op);
  for (uint16_t i = 0; i < function.op->tied_result_count; ++i) {
    if (loom_view_boundary_tie_uses_expanded_slot(out_function, &ties[i])) {
      out_function->selected = false;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_plan_functions(
    loom_view_boundary_plan_t* plan,
    const loom_function_version_list_t* version_list) {
  plan->function_index_count = plan->module->symbols.count;
  if (plan->function_index_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->function_index_count, sizeof(*plan->function_indices),
      (void**)&plan->function_indices));
  for (iree_host_size_t i = 0; i < plan->function_index_count; ++i) {
    plan->function_indices[i] = IREE_HOST_SIZE_MAX;
  }
  IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
      plan->module, version_list, plan->arena, &plan->versions));

  iree_host_size_t function_count = 0;
  for (iree_host_size_t symbol_id = 0; symbol_id < plan->module->symbols.count;
       ++symbol_id) {
    const loom_target_function_version_t* version =
        loom_target_function_version_snapshot_at(&plan->versions, symbol_id);
    const loom_symbol_t* symbol = &plan->module->symbols.entries[symbol_id];
    if (!loom_view_boundary_provider_uses_buffer_offset(version) ||
        !loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_FUNC_LIKE) ||
        symbol->defining_op == NULL) {
      continue;
    }
    ++function_count;
  }
  if (function_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(plan->arena, function_count,
                                                 sizeof(*plan->functions),
                                                 (void**)&plan->functions));
  memset(plan->functions, 0, function_count * sizeof(*plan->functions));

  for (iree_host_size_t symbol_id = 0; symbol_id < plan->module->symbols.count;
       ++symbol_id) {
    loom_function_version_t* version =
        loom_target_function_version_snapshot_handle_at(&plan->versions,
                                                        symbol_id);
    const loom_target_function_version_t* target_version =
        loom_target_function_version_const_cast(version);
    loom_symbol_t* symbol = &plan->module->symbols.entries[symbol_id];
    if (!loom_view_boundary_provider_uses_buffer_offset(target_version) ||
        !loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_FUNC_LIKE) ||
        symbol->defining_op == NULL) {
      continue;
    }
    const iree_host_size_t index = plan->function_count++;
    plan->function_indices[symbol_id] = index;
    IREE_RETURN_IF_ERROR(loom_view_boundary_plan_function_signature(
        plan, loom_func_like_cast(plan->module, symbol->defining_op), version,
        &plan->functions[index]));
  }
  IREE_ASSERT_EQ(plan->function_count, function_count);
  return iree_ok_status();
}

static iree_host_size_t loom_view_boundary_function_index(
    const loom_view_boundary_plan_t* plan, loom_symbol_ref_t symbol) {
  return loom_symbol_ref_is_valid(symbol) && symbol.module_id == 0 &&
                 symbol.symbol_id < plan->function_index_count
             ? plan->function_indices[symbol.symbol_id]
             : IREE_HOST_SIZE_MAX;
}

static iree_status_t loom_view_boundary_add_candidate(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function,
    loom_value_id_t value_id, loom_view_boundary_candidate_kind_t kind,
    loom_block_t* block, loom_op_t* call_op) {
  if (!loom_type_is_view(loom_module_value_type(plan->module, value_id))) {
    return iree_ok_status();
  }
  if (function->candidate_count == function->candidate_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, function->candidate_count, function->candidate_count + 1,
        sizeof(*function->candidates), &function->candidate_capacity,
        (void**)&function->candidates));
  }
  function->candidates[function->candidate_count++] =
      (loom_view_boundary_candidate_t){
          .value_id = value_id,
          .view_type = loom_module_value_type(plan->module, value_id),
          .block = block,
          .call_op = call_op,
          .buffer_value_id = LOOM_VALUE_ID_INVALID,
          .offset_value_id = LOOM_VALUE_ID_INVALID,
          .replacement_value_id = LOOM_VALUE_ID_INVALID,
          .kind = kind,
      };
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_add_call(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function,
    loom_call_like_t call, iree_host_size_t callee_index) {
  if (function->call_count == function->call_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, function->call_count, function->call_count + 1,
        sizeof(*function->calls), &function->call_capacity,
        (void**)&function->calls));
  }
  function->calls[function->call_count++] = (loom_view_boundary_call_t){
      .call = call,
      .callee_index = callee_index,
  };
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_add_return(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function,
    loom_op_t* op) {
  if (function->return_count == function->return_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, function->return_count, function->return_count + 1,
        sizeof(*function->returns), &function->return_capacity,
        (void**)&function->returns));
  }
  function->returns[function->return_count++] =
      (loom_view_boundary_return_t){.op = op};
  return iree_ok_status();
}

typedef struct loom_view_boundary_collect_t {
  // Whole-module plan used to resolve callees.
  loom_view_boundary_plan_t* plan;
  // Function currently being walked.
  loom_view_boundary_function_t* function;
} loom_view_boundary_collect_t;

static int loom_view_boundary_candidate_compare(const void* lhs,
                                                const void* rhs) {
  const loom_view_boundary_candidate_t* left = lhs;
  const loom_view_boundary_candidate_t* right = rhs;
  return left->value_id < right->value_id   ? -1
         : left->value_id > right->value_id ? 1
                                            : 0;
}

static iree_status_t loom_view_boundary_collect_op(
    void* user_data, loom_op_t* op, const loom_walk_context_t* context,
    loom_walk_result_t* out_result) {
  (void)context;
  *out_result = LOOM_WALK_CONTINUE;
  loom_view_boundary_collect_t* collect = user_data;

  if (collect->function->result_signature_changes &&
      collect->function->return_kind != LOOM_OP_KIND_UNKNOWN &&
      op->kind == collect->function->return_kind &&
      op->parent_op == collect->function->function.op) {
    return loom_view_boundary_add_return(collect->plan, collect->function, op);
  }

  const loom_call_like_t call = loom_call_like_cast(collect->plan->module, op);
  if (!loom_call_like_isa(call) ||
      loom_call_like_kind(call) != LOOM_CALL_LIKE_KIND_SEMANTIC) {
    return iree_ok_status();
  }
  const loom_symbol_ref_t callee = loom_call_like_callee(call);
  const loom_value_slice_t operands = loom_call_like_operands(call);
  const loom_value_slice_t results = loom_call_like_results(call);
  const iree_host_size_t callee_index =
      loom_view_boundary_function_index(collect->plan, callee);
  if (callee_index == IREE_HOST_SIZE_MAX) {
    return iree_ok_status();
  }
  loom_view_boundary_function_t* callee_plan =
      &collect->plan->functions[callee_index];
  if (!callee_plan->signature_changes) {
    return iree_ok_status();
  }
  if (operands.count != callee_plan->argument_count ||
      results.count != callee_plan->result_count) {
    collect->function->selected = false;
    callee_plan->selected = false;
    return iree_ok_status();
  }
  if (op->region_count != 0 || op->successor_count != 0) {
    collect->function->selected = false;
    callee_plan->selected = false;
    return iree_ok_status();
  }
  if ((uint32_t)loom_call_like_operand_offset(call) +
              callee_plan->final_argument_count >
          UINT16_MAX ||
      (uint32_t)loom_call_like_result_offset(call) +
              callee_plan->final_result_count >
          UINT16_MAX) {
    collect->function->selected = false;
    callee_plan->selected = false;
    return iree_ok_status();
  }
  const loom_tied_result_t* ties = loom_op_tied_results(op);
  for (uint16_t i = 0; i < op->tied_result_count; ++i) {
    if (loom_view_boundary_call_tie_uses_expanded_slot(call, callee_plan,
                                                       &ties[i])) {
      collect->function->selected = false;
      callee_plan->selected = false;
      return iree_ok_status();
    }
  }
  IREE_RETURN_IF_ERROR(loom_view_boundary_add_call(
      collect->plan, collect->function, call, callee_index));
  for (uint16_t i = 0; i < results.count; ++i) {
    if (!callee_plan->view_results[i]) {
      continue;
    }
    if (loom_view_boundary_value_has_nonoperand_uses(collect->plan->module,
                                                     results.values[i])) {
      collect->function->selected = false;
      callee_plan->selected = false;
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_view_boundary_add_candidate(
        collect->plan, collect->function, results.values[i],
        LOOM_VIEW_BOUNDARY_CANDIDATE_CALL_RESULT, NULL, op));
  }
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_collect_function(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function) {
  loom_region_t* body = loom_func_like_body(function->function);
  if (!body) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_local_value_domain_acquire_for_region_tree(
      plan->module, body, plan->arena, &function->domain));

  loom_block_t* block = NULL;
  loom_region_for_each_block(body, block) {
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      IREE_RETURN_IF_ERROR(loom_view_boundary_add_candidate(
          plan, function, loom_block_arg_id(block, i),
          LOOM_VIEW_BOUNDARY_CANDIDATE_BLOCK_ARGUMENT, block, NULL));
    }
  }

  loom_view_boundary_collect_t collect = {
      .plan = plan,
      .function = function,
  };
  loom_walk_result_t walk_result = LOOM_WALK_CONTINUE;
  IREE_RETURN_IF_ERROR(loom_walk_function(
      plan->module, function->function, LOOM_WALK_PRE_ORDER,
      (loom_walk_callback_t){loom_view_boundary_collect_op, &collect},
      plan->arena, &walk_result));

  if (function->candidate_count == 0 && function->call_count == 0 &&
      !function->signature_changes) {
    function->selected = false;
    return iree_ok_status();
  }
  if (function->candidate_count > 1) {
    qsort(function->candidates, function->candidate_count,
          sizeof(*function->candidates), loom_view_boundary_candidate_compare);
    for (iree_host_size_t i = 1; i < function->candidate_count; ++i) {
      if (function->candidates[i - 1].value_id ==
          function->candidates[i].value_id) {
        function->selected = false;
      }
    }
  }
  return iree_ok_status();
}

iree_host_size_t loom_view_boundary_candidate_index(
    const loom_view_boundary_function_t* function, loom_value_id_t value_id) {
  iree_host_size_t begin = 0;
  iree_host_size_t end = function->candidate_count;
  while (begin < end) {
    const iree_host_size_t middle = begin + (end - begin) / 2;
    const loom_value_id_t candidate = function->candidates[middle].value_id;
    if (candidate < value_id) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return begin < function->candidate_count &&
                 function->candidates[begin].value_id == value_id
             ? begin
             : IREE_HOST_SIZE_MAX;
}

static void loom_view_boundary_plan_offset(
    const loom_view_boundary_function_t* function,
    const loom_view_region_t* region, loom_view_boundary_offset_t* offset) {
  *offset = (loom_view_boundary_offset_t){
      .anchor_value_id = region->view_value_id,
      .value_id = region->begin_value_id,
      .base_value_id = LOOM_VALUE_ID_INVALID,
      .dependency = IREE_HOST_SIZE_MAX,
  };
  if (offset->value_id != LOOM_VALUE_ID_INVALID) {
    return;
  }
  if (loom_symbolic_expr_is_linear(&region->projection_byte_offset)) {
    const iree_host_size_t dependency = loom_view_boundary_candidate_index(
        function, region->base_view_value_id);
    if (dependency != IREE_HOST_SIZE_MAX) {
      offset->dependency = dependency;
    } else {
      const loom_view_region_t* base_region = NULL;
      if (loom_view_region_table_try_lookup(
              &function->regions, region->base_view_value_id, &base_region)) {
        offset->base_value_id = base_region->begin_value_id;
      }
    }
    if (offset->dependency != IREE_HOST_SIZE_MAX ||
        offset->base_value_id != LOOM_VALUE_ID_INVALID) {
      offset->expression = &region->projection_byte_offset;
      return;
    }
  }
  if (loom_symbolic_expr_is_linear(&region->begin_byte_offset)) {
    offset->expression = &region->begin_byte_offset;
  }
}

static iree_status_t loom_view_boundary_analyze_function(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function) {
  if (!function->selected || !loom_func_like_body(function->function)) {
    return iree_ok_status();
  }
  const loom_target_function_version_t* target_version =
      loom_target_function_version_const_cast(function->version);
  IREE_ASSERT(target_version != NULL);
  IREE_RETURN_IF_ERROR(loom_pass_value_facts_acquire(
      plan->pass, plan->module,
      loom_pass_value_fact_scope_function_for_target(
          function->function, target_version->function_target_facts),
      &function->facts));

  if (function->domain.value_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(plan->arena, function->domain.value_count,
                                  sizeof(*function->selection_indices),
                                  (void**)&function->selection_indices));
    for (loom_value_ordinal_t i = 0; i < function->domain.value_count; ++i) {
      function->selection_indices[i] = IREE_HOST_SIZE_MAX;
    }
  }

  loom_symbolic_expr_context_initialize(plan->module, &function->domain,
                                        function->facts, plan->arena,
                                        &function->expressions);
  IREE_RETURN_IF_ERROR(loom_view_region_table_initialize(
      &function->domain, &function->expressions, &function->regions));
  IREE_RETURN_IF_ERROR(loom_view_region_table_analyze(&function->regions));
  if (function->regions.region_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, function->regions.region_count, sizeof(*function->offsets),
      (void**)&function->offsets));
  for (iree_host_size_t i = 0; i < function->regions.region_count; ++i) {
    loom_view_boundary_plan_offset(function, &function->regions.regions[i],
                                   &function->offsets[i]);
  }
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_plan_coordinate(
    const loom_view_boundary_plan_t* plan,
    loom_view_boundary_function_t* function, loom_value_id_t source,
    loom_view_boundary_coordinate_t* out_coordinate, bool* out_planned);

static bool loom_view_boundary_match_binary_selection(
    const loom_module_t* module, loom_value_id_t result_value_id,
    loom_op_t** out_op, loom_value_id_t* out_condition_value_id,
    loom_value_id_t* out_true_value_id, loom_value_id_t* out_false_value_id) {
  const loom_value_t* result_value = loom_module_value(module, result_value_id);
  if (loom_value_is_block_arg(result_value)) {
    return false;
  }
  loom_op_t* op = loom_value_def_op(result_value);
  const uint16_t result_index = loom_value_def_index(result_value);
  if (!op || op->result_count == 0 || result_index >= op->result_count) {
    return false;
  }

  const loom_op_vtable_t* vtable = loom_op_vtable(module, op);
  loom_value_id_t condition_value_id = LOOM_VALUE_ID_INVALID;
  loom_value_id_t true_value_id = LOOM_VALUE_ID_INVALID;
  loom_value_id_t false_value_id = LOOM_VALUE_ID_INVALID;
  uint16_t condition_count = 0;
  uint32_t payload_count = 0;
  const loom_value_id_t* operands = loom_op_const_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    const loom_operand_role_t role = loom_op_operand_role_at(vtable, op, i);
    if (role == LOOM_OPERAND_ROLE_SELECT_CONDITION) {
      condition_value_id = operands[i];
      ++condition_count;
    } else if (role == LOOM_OPERAND_ROLE_SELECT_PAYLOAD) {
      const uint32_t payload_result_index = payload_count % op->result_count;
      const uint32_t payload_group_index = payload_count / op->result_count;
      if (payload_result_index == result_index) {
        if (payload_group_index == 0) {
          true_value_id = operands[i];
        } else if (payload_group_index == 1) {
          false_value_id = operands[i];
        }
      }
      ++payload_count;
    }
  }
  if (condition_count != 1 || payload_count != 2u * op->result_count ||
      true_value_id == LOOM_VALUE_ID_INVALID ||
      false_value_id == LOOM_VALUE_ID_INVALID ||
      !loom_type_equal(loom_module_value_type(module, condition_value_id),
                       loom_type_scalar(LOOM_SCALAR_TYPE_I1))) {
    return false;
  }
  *out_op = op;
  *out_condition_value_id = condition_value_id;
  *out_true_value_id = true_value_id;
  *out_false_value_id = false_value_id;
  return true;
}

static iree_status_t loom_view_boundary_plan_selection(
    const loom_view_boundary_plan_t* plan,
    loom_view_boundary_function_t* function, loom_op_t* op,
    loom_value_id_t result_value_id, loom_value_id_t condition_value_id,
    loom_value_id_t true_value_id, loom_value_id_t false_value_id,
    loom_view_boundary_coordinate_t* out_coordinate, bool* out_planned) {
  const loom_value_ordinal_t result_ordinal =
      loom_local_value_domain_ordinal(&function->domain, result_value_id);
  iree_host_size_t selection_index =
      function->selection_indices[result_ordinal];
  if (selection_index != IREE_HOST_SIZE_MAX) {
    const loom_view_boundary_selection_t* selection =
        &function->selections[selection_index];
    *out_planned = selection->selected && !selection->planning;
    if (*out_planned) {
      out_coordinate->selection = selection_index;
    }
    return iree_ok_status();
  }

  if (function->selection_count == function->selection_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, function->selection_count, function->selection_count + 1,
        sizeof(*function->selections), &function->selection_capacity,
        (void**)&function->selections));
  }
  selection_index = function->selection_count++;
  function->selection_indices[result_ordinal] = selection_index;
  function->selections[selection_index] = (loom_view_boundary_selection_t){
      .op = op,
      .result_value_id = result_value_id,
      .condition_value_id = condition_value_id,
      .true_value_id = true_value_id,
      .false_value_id = false_value_id,
      .buffer_value_id = LOOM_VALUE_ID_INVALID,
      .offset_value_id = LOOM_VALUE_ID_INVALID,
      .planning = true,
  };

  // Recursive planning may grow and relocate the selection array. Keep the
  // alternatives local until both recursive calls have completed.
  loom_view_boundary_coordinate_t true_coordinate;
  bool true_planned = false;
  IREE_RETURN_IF_ERROR(loom_view_boundary_plan_coordinate(
      plan, function, true_value_id, &true_coordinate, &true_planned));
  loom_view_boundary_coordinate_t false_coordinate;
  bool false_planned = false;
  IREE_RETURN_IF_ERROR(loom_view_boundary_plan_coordinate(
      plan, function, false_value_id, &false_coordinate, &false_planned));
  loom_view_boundary_selection_t* selection =
      &function->selections[selection_index];
  selection->true_coordinate = true_coordinate;
  selection->false_coordinate = false_coordinate;
  selection->planning = false;
  selection->selected = true_planned && false_planned;
  *out_planned = selection->selected;
  if (*out_planned) {
    out_coordinate->selection = selection_index;
  }
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_plan_coordinate(
    const loom_view_boundary_plan_t* plan,
    loom_view_boundary_function_t* function, loom_value_id_t source,
    loom_view_boundary_coordinate_t* out_coordinate, bool* out_planned) {
  *out_coordinate = (loom_view_boundary_coordinate_t){
      .buffer_value_id = LOOM_VALUE_ID_INVALID,
      .region_id = LOOM_VIEW_REGION_ID_INVALID,
      .dependency = IREE_HOST_SIZE_MAX,
      .selection = IREE_HOST_SIZE_MAX,
  };
  *out_planned = false;
  const iree_host_size_t direct_candidate =
      loom_view_boundary_candidate_index(function, source);
  if (direct_candidate != IREE_HOST_SIZE_MAX) {
    out_coordinate->dependency = direct_candidate;
    *out_planned = true;
    return iree_ok_status();
  }

  loom_op_t* selection_op = NULL;
  loom_value_id_t condition_value_id = LOOM_VALUE_ID_INVALID;
  loom_value_id_t true_value_id = LOOM_VALUE_ID_INVALID;
  loom_value_id_t false_value_id = LOOM_VALUE_ID_INVALID;
  if (loom_view_boundary_match_binary_selection(
          plan->module, source, &selection_op, &condition_value_id,
          &true_value_id, &false_value_id)) {
    return loom_view_boundary_plan_selection(
        plan, function, selection_op, source, condition_value_id, true_value_id,
        false_value_id, out_coordinate, out_planned);
  }

  const loom_view_region_t* region = NULL;
  if (!loom_view_region_table_try_lookup(&function->regions, source, &region)) {
    return iree_ok_status();
  }
  const loom_view_boundary_offset_t* offset =
      &function->offsets[region->region_id];
  if (offset->value_id == LOOM_VALUE_ID_INVALID && !offset->expression) {
    return iree_ok_status();
  }
  out_coordinate->region_id = region->region_id;
  if (offset->dependency != IREE_HOST_SIZE_MAX) {
    out_coordinate->dependency = offset->dependency;
    *out_planned = true;
    return iree_ok_status();
  }

  loom_value_fact_view_reference_t reference;
  if (!loom_value_facts_query_view_reference(
          &function->facts->context,
          loom_value_fact_table_lookup(function->facts, source), &reference) ||
      reference.buffer_value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  out_coordinate->buffer_value_id = reference.buffer_value_id;
  *out_planned = true;
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_plan_call_coordinates(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function) {
  for (iree_host_size_t call_index = 0; call_index < function->call_count;
       ++call_index) {
    loom_view_boundary_call_t* call = &function->calls[call_index];
    loom_view_boundary_function_t* callee =
        &plan->functions[call->callee_index];
    const loom_value_slice_t operands = loom_call_like_operands(call->call);
    if (operands.count != 0) {
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          plan->arena, operands.count, sizeof(*call->operand_coordinates),
          (void**)&call->operand_coordinates));
    }
    for (uint16_t i = 0; i < operands.count; ++i) {
      call->operand_coordinates[i] = (loom_view_boundary_coordinate_t){
          .buffer_value_id = LOOM_VALUE_ID_INVALID,
          .region_id = LOOM_VIEW_REGION_ID_INVALID,
          .dependency = IREE_HOST_SIZE_MAX,
          .selection = IREE_HOST_SIZE_MAX,
      };
      bool planned = false;
      if (callee->view_arguments[i]) {
        IREE_RETURN_IF_ERROR(loom_view_boundary_plan_coordinate(
            plan, function, operands.values[i], &call->operand_coordinates[i],
            &planned));
      }
      if (callee->view_arguments[i] && !planned) {
        function->selected = false;
        callee->selected = false;
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_view_boundary_plan_return_coordinates(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function) {
  for (iree_host_size_t return_index = 0; return_index < function->return_count;
       ++return_index) {
    loom_view_boundary_return_t* return_plan = &function->returns[return_index];
    const loom_value_slice_t operands = {
        .values = loom_op_operands(return_plan->op),
        .count = return_plan->op->operand_count,
    };
    if (operands.count != function->result_count) {
      function->selected = false;
      continue;
    }
    const loom_op_vtable_t* vtable =
        loom_op_vtable(plan->module, return_plan->op);
    if (return_plan->op->result_count != 0 ||
        return_plan->op->region_count != 0 ||
        return_plan->op->successor_count != 0 ||
        return_plan->op->tied_result_count != 0 ||
        loom_op_vtable_has_segmented_operands(vtable)) {
      function->selected = false;
      continue;
    }
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, operands.count, sizeof(*return_plan->coordinates),
        (void**)&return_plan->coordinates));
    for (uint16_t i = 0; i < operands.count; ++i) {
      return_plan->coordinates[i] = (loom_view_boundary_coordinate_t){
          .buffer_value_id = LOOM_VALUE_ID_INVALID,
          .region_id = LOOM_VIEW_REGION_ID_INVALID,
          .dependency = IREE_HOST_SIZE_MAX,
          .selection = IREE_HOST_SIZE_MAX,
      };
      bool planned = false;
      if (function->view_results[i]) {
        IREE_RETURN_IF_ERROR(loom_view_boundary_plan_coordinate(
            plan, function, operands.values[i], &return_plan->coordinates[i],
            &planned));
      }
      if (function->view_results[i] && !planned) {
        function->selected = false;
      }
    }
  }
  return iree_ok_status();
}

static bool loom_view_boundary_block_has_candidate(
    const loom_view_boundary_function_t* function, const loom_block_t* block) {
  for (uint16_t i = 0; i < block->arg_count; ++i) {
    if (loom_view_boundary_candidate_index(
            function, loom_block_arg_id(block, i)) != IREE_HOST_SIZE_MAX) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_view_boundary_plan_blocks(
    loom_view_boundary_plan_t* plan, loom_view_boundary_function_t* function) {
  loom_region_t* body = loom_func_like_body(function->function);
  if (!body || function->candidate_count == 0) {
    return iree_ok_status();
  }
  const loom_block_t* entry = loom_region_entry_block(body);
  bool has_non_entry_block_candidate = false;
  for (iree_host_size_t i = 0; i < function->candidate_count; ++i) {
    const loom_view_boundary_candidate_t* candidate = &function->candidates[i];
    if (candidate->kind == LOOM_VIEW_BOUNDARY_CANDIDATE_BLOCK_ARGUMENT &&
        candidate->block != entry) {
      has_non_entry_block_candidate = true;
      break;
    }
  }
  if (!has_non_entry_block_candidate) {
    return iree_ok_status();
  }
  const loom_value_fact_cfg_region_t* cfg =
      loom_value_fact_table_lookup_cfg_region(function->facts, body);
  if (!cfg || cfg->graph.malformed) {
    function->selected = false;
    return iree_ok_status();
  }

  iree_host_size_t block_count = 0;
  for (uint16_t block_index = 0; block_index < cfg->graph.block_count;
       ++block_index) {
    const loom_block_t* block = cfg->graph.blocks[block_index].block;
    if (loom_view_boundary_block_has_candidate(function, block)) {
      ++block_count;
    }
  }
  if (block_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(plan->arena, block_count,
                                                 sizeof(*function->blocks),
                                                 (void**)&function->blocks));
  memset(function->blocks, 0, block_count * sizeof(*function->blocks));

  for (uint16_t block_index = 0; block_index < cfg->graph.block_count;
       ++block_index) {
    loom_block_t* block = (loom_block_t*)cfg->graph.blocks[block_index].block;
    if (!loom_view_boundary_block_has_candidate(function, block)) {
      continue;
    }
    loom_view_boundary_block_t* block_plan =
        &function->blocks[function->block_count++];
    block_plan->block = block;
    block_plan->original_argument_count = block->arg_count;
    block_plan->final_argument_count = block->arg_count;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, block->arg_count, sizeof(*block_plan->original_arguments),
        (void**)&block_plan->original_arguments));
    memcpy(block_plan->original_arguments, block->arg_ids,
           block->arg_count * sizeof(*block->arg_ids));
    for (uint16_t i = 0; i < block->arg_count; ++i) {
      if (loom_view_boundary_candidate_index(
              function, block_plan->original_arguments[i]) !=
          IREE_HOST_SIZE_MAX) {
        if (block_plan->final_argument_count == UINT16_MAX) {
          function->selected = false;
          continue;
        }
        ++block_plan->final_argument_count;
      }
    }
    if (block_plan->final_argument_count < block->arg_count) {
      function->selected = false;
      continue;
    }

    if (block_index == 0) {
      continue;
    }
    const loom_cfg_edge_index_span_t predecessor_edges =
        loom_cfg_graph_predecessor_edges(&cfg->graph, block_index);
    block_plan->edge_count = predecessor_edges.count;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, predecessor_edges.count, sizeof(*block_plan->edges),
        (void**)&block_plan->edges));
    memset(block_plan->edges, 0,
           predecessor_edges.count * sizeof(*block_plan->edges));
    for (iree_host_size_t edge_index = 0; edge_index < predecessor_edges.count;
         ++edge_index) {
      const loom_cfg_edge_info_t* edge = loom_cfg_graph_edge(
          &cfg->graph, predecessor_edges.values[edge_index]);
      loom_op_t* terminator = edge ? (loom_op_t*)edge->terminator : NULL;
      const loom_value_id_t* sources = NULL;
      uint16_t source_count = 0;
      const loom_op_vtable_t* vtable =
          terminator ? loom_op_vtable(plan->module, terminator) : NULL;
      if (!loom_cfg_terminator_payload_for_successor(terminator, block,
                                                     &sources, &source_count) ||
          source_count != block->arg_count || terminator->result_count != 0 ||
          terminator->region_count != 0 || terminator->tied_result_count != 0 ||
          loom_op_vtable_has_segmented_operands(vtable)) {
        function->selected = false;
        continue;
      }
      loom_view_boundary_edge_t* edge_plan = &block_plan->edges[edge_index];
      edge_plan->terminator = terminator;
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          plan->arena, block->arg_count, sizeof(*edge_plan->coordinates),
          (void**)&edge_plan->coordinates));
      for (uint16_t i = 0; i < block->arg_count; ++i) {
        edge_plan->coordinates[i] = (loom_view_boundary_coordinate_t){
            .buffer_value_id = LOOM_VALUE_ID_INVALID,
            .region_id = LOOM_VIEW_REGION_ID_INVALID,
            .dependency = IREE_HOST_SIZE_MAX,
            .selection = IREE_HOST_SIZE_MAX,
        };
        if (loom_view_boundary_candidate_index(
                function, block_plan->original_arguments[i]) ==
            IREE_HOST_SIZE_MAX) {
          continue;
        }
        bool planned = false;
        IREE_RETURN_IF_ERROR(loom_view_boundary_plan_coordinate(
            plan, function, sources[i], &edge_plan->coordinates[i], &planned));
        if (!planned) {
          function->selected = false;
        }
      }
    }
  }
  IREE_ASSERT_EQ(function->block_count, block_count);
  return iree_ok_status();
}

static iree_host_size_t loom_view_boundary_component_root(
    iree_host_size_t* parents, iree_host_size_t index) {
  iree_host_size_t root = index;
  while (parents[root] != root) {
    root = parents[root];
  }
  while (parents[index] != index) {
    const iree_host_size_t parent = parents[index];
    parents[index] = root;
    index = parent;
  }
  return root;
}

static iree_status_t loom_view_boundary_propagate_rejections(
    loom_view_boundary_plan_t* plan) {
  if (plan->function_count == 0) {
    return iree_ok_status();
  }
  iree_host_size_t* parents = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->function_count, sizeof(*parents), (void**)&parents));
  iree_host_size_t* component_sizes = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->function_count, sizeof(*component_sizes),
      (void**)&component_sizes));
  bool* component_selected = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, plan->function_count, sizeof(*component_selected),
      (void**)&component_selected));
  for (iree_host_size_t i = 0; i < plan->function_count; ++i) {
    parents[i] = i;
    component_sizes[i] = 1;
    component_selected[i] = true;
  }

  // Every rewritten semantic call couples its caller and callee signatures.
  // Union the undirected call components once so rejection is linear in the
  // planned call graph rather than repeatedly rescanning long call chains.
  for (iree_host_size_t i = 0; i < plan->function_count; ++i) {
    const loom_view_boundary_function_t* caller = &plan->functions[i];
    for (iree_host_size_t j = 0; j < caller->call_count; ++j) {
      iree_host_size_t caller_root =
          loom_view_boundary_component_root(parents, i);
      iree_host_size_t callee_root = loom_view_boundary_component_root(
          parents, caller->calls[j].callee_index);
      if (caller_root == callee_root) {
        continue;
      }
      if (component_sizes[caller_root] < component_sizes[callee_root]) {
        const iree_host_size_t temporary = caller_root;
        caller_root = callee_root;
        callee_root = temporary;
      }
      parents[callee_root] = caller_root;
      component_sizes[caller_root] += component_sizes[callee_root];
    }
  }
  for (iree_host_size_t i = 0; i < plan->function_count; ++i) {
    const iree_host_size_t root = loom_view_boundary_component_root(parents, i);
    component_selected[root] &= plan->functions[i].selected;
  }
  for (iree_host_size_t i = 0; i < plan->function_count; ++i) {
    const iree_host_size_t root = loom_view_boundary_component_root(parents, i);
    plan->functions[i].selected = component_selected[root];
  }
  return iree_ok_status();
}

iree_status_t loom_view_boundary_plan_prepare(
    loom_view_boundary_plan_t* plan,
    const loom_function_version_list_t* version_list) {
  IREE_RETURN_IF_ERROR(loom_view_boundary_plan_functions(plan, version_list));
  for (iree_host_size_t i = 0; i < plan->function_count; ++i) {
    loom_view_boundary_function_t* function = &plan->functions[i];
    iree_status_t status = loom_view_boundary_collect_function(plan, function);
    if (iree_status_is_ok(status)) {
      status = loom_view_boundary_analyze_function(plan, function);
    }
    if (iree_status_is_ok(status) && function->selected &&
        loom_func_like_body(function->function)) {
      status = loom_view_boundary_plan_call_coordinates(plan, function);
    }
    if (iree_status_is_ok(status) && function->selected &&
        loom_func_like_body(function->function)) {
      status = loom_view_boundary_plan_return_coordinates(plan, function);
    }
    if (iree_status_is_ok(status) && function->selected &&
        loom_func_like_body(function->function)) {
      status = loom_view_boundary_plan_blocks(plan, function);
    }
    loom_local_value_domain_release(&function->domain);
    IREE_RETURN_IF_ERROR(status);
  }
  return loom_view_boundary_propagate_rejections(plan);
}
