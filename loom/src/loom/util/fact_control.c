// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_control.h"

#include <string.h>

#define LOOM_VALUE_FACT_CONTROL_TOP 4

static uint8_t loom_value_fact_control_distribution(loom_value_facts_t facts) {
  if (loom_value_facts_is_lane_varying(facts) ||
      loom_value_facts_is_lane_predicate(facts)) {
    return 0;
  }
  return 1 + loom_value_facts_uniform_scope(facts);
}

static void loom_value_fact_control_link_input(
    loom_value_fact_control_t* control, uint32_t index, uint8_t distribution) {
  loom_value_fact_control_input_t* input = &control->inputs[index];
  loom_value_fact_control_component_t* component =
      &control->components[control->structure->inputs[index].target_component];
  input->distribution = distribution;
  input->previous = LOOM_CFG_CONTROL_INVALID;
  input->next = component->heads[distribution];
  if (input->next != LOOM_CFG_CONTROL_INVALID) {
    control->inputs[input->next].previous = index;
  }
  component->heads[distribution] = index;
}

static void loom_value_fact_control_queue(loom_value_fact_control_t* control,
                                          uint32_t component) {
  if (control->components[component].queued) {
    return;
  }
  control->components[component].queued = true;
  uint32_t position = control->worklist_count++;
  while (position && control->worklist[(position - 1) / 2] < component) {
    control->worklist[position] = control->worklist[(position - 1) / 2];
    position = (position - 1) / 2;
  }
  control->worklist[position] = component;
}

static uint32_t loom_value_fact_control_pop(
    loom_value_fact_control_t* control) {
  const uint32_t component = control->worklist[0];
  const uint32_t last = control->worklist[--control->worklist_count];
  uint32_t position = 0;
  while (2 * position + 1 < control->worklist_count) {
    uint32_t child = 2 * position + 1;
    if (child + 1 < control->worklist_count &&
        control->worklist[child + 1] > control->worklist[child]) {
      ++child;
    }
    if (last >= control->worklist[child]) {
      break;
    }
    control->worklist[position] = control->worklist[child];
    position = child;
  }
  control->worklist[position] = last;
  control->components[component].queued = false;
  return component;
}

static void loom_value_fact_control_update_input(
    loom_value_fact_control_t* control, uint32_t index, uint8_t distribution) {
  loom_value_fact_control_input_t* input = &control->inputs[index];
  const uint32_t target = control->structure->inputs[index].target_component;
  if (input->distribution == distribution) {
    return;
  }
  if (input->previous != LOOM_CFG_CONTROL_INVALID) {
    control->inputs[input->previous].next = input->next;
  } else {
    control->components[target].heads[input->distribution] = input->next;
  }
  if (input->next != LOOM_CFG_CONTROL_INVALID) {
    control->inputs[input->next].previous = input->previous;
  }
  loom_value_fact_control_link_input(control, index, distribution);
  loom_value_fact_control_queue(control, target);
}

iree_status_t loom_value_fact_control_initialize(
    const loom_cfg_control_t* structure, iree_arena_allocator_t* arena,
    loom_value_fact_control_t* out_control) {
  *out_control = (loom_value_fact_control_t){
      .structure = structure,
      .diagnostics_valid = true,
  };
  if (!structure->node_count) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, structure->input_count,
                                                 sizeof(*out_control->inputs),
                                                 (void**)&out_control->inputs));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, structure->component_count, sizeof(*out_control->components),
      (void**)&out_control->components));
  for (uint32_t i = 0; i < structure->component_count; ++i) {
    loom_value_fact_control_component_t* component =
        &out_control->components[i];
    memset(component->heads, 0xFF, sizeof(component->heads));
    component->controller = LOOM_CFG_EDGE_INDEX_INVALID;
    component->distribution = LOOM_VALUE_FACT_CONTROL_TOP;
    component->queued = false;
  }
  for (uint32_t i = 0; i < structure->input_count; ++i) {
    loom_value_fact_control_link_input(out_control, i,
                                       LOOM_VALUE_FACT_CONTROL_TOP);
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, structure->graph->block_count, sizeof(*out_control->blocks),
      (void**)&out_control->blocks));
  for (uint32_t i = 0; i < structure->graph->block_count; ++i) {
    out_control->blocks[i] = (loom_value_fact_control_block_t){
        .selector_distribution = LOOM_VALUE_FACT_CONTROL_TOP,
    };
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, structure->component_count, sizeof(*out_control->worklist),
      (void**)&out_control->worklist));
  return iree_arena_allocate_array(arena, structure->graph->block_count,
                                   sizeof(*out_control->pending_blocks),
                                   (void**)&out_control->pending_blocks);
}

void loom_value_fact_control_set_selector(loom_value_fact_control_t* control,
                                          uint16_t block_index,
                                          loom_value_facts_t facts) {
  if (!control->structure->node_count) {
    return;
  }
  const uint8_t distribution = loom_value_fact_control_distribution(facts);
  loom_value_fact_control_block_t* block = &control->blocks[block_index];
  if (block->selector_distribution == distribution) {
    return;
  }
  block->selector_distribution = distribution;
  control->diagnostics_valid = false;
  const loom_cfg_control_block_t* source =
      &control->structure->blocks[block_index];
  for (uint32_t i = source->binding_start;
       i < source->binding_start + source->binding_count; ++i) {
    loom_value_fact_control_update_input(
        control, control->structure->bindings[i].selector_input, distribution);
  }
}

bool loom_value_fact_control_settle(loom_value_fact_control_t* control) {
  const loom_cfg_control_t* structure = control->structure;
  bool changed = false;
  while (control->worklist_count) {
    const uint32_t index = loom_value_fact_control_pop(control);
    loom_value_fact_control_component_t* component =
        &control->components[index];
    uint8_t distribution = 0;
    while (distribution < LOOM_VALUE_FACT_CONTROL_TOP &&
           component->heads[distribution] == LOOM_CFG_CONTROL_INVALID) {
      ++distribution;
    }
    if (distribution == component->distribution) {
      continue;
    }
    changed = true;
    for (uint32_t i = structure->components[index].block_head;
         i != LOOM_CFG_CONTROL_INVALID;
         i = structure->blocks[i].next_component_block) {
      loom_value_fact_control_block_t* block = &control->blocks[i];
      if (!block->pending) {
        block->pending = true;
        block->old_distribution = component->distribution;
        control->pending_blocks[control->pending_count++] = i;
      }
    }
    component->distribution = distribution;
    for (uint32_t i = structure->components[index].outgoing_head;
         i != LOOM_CFG_CONTROL_INVALID;
         i = structure->inputs[i].next_outgoing) {
      loom_value_fact_control_update_input(control, i, distribution);
    }
  }
  return changed;
}

loom_value_facts_t loom_value_fact_control_execution(
    const loom_value_fact_control_t* control, uint16_t block_index) {
  loom_value_facts_t facts = loom_value_facts_unknown();
  const loom_cfg_control_t* structure = control->structure;
  if (!structure->available ||
      !structure->graph->blocks[block_index].reachable) {
    return facts;
  }
  const uint8_t distribution =
      structure->node_count
          ? control
                ->components[structure
                                 ->nodes[structure->blocks[block_index].node]
                                 .component]
                .distribution
          : LOOM_VALUE_FACT_CONTROL_TOP;
  if (!distribution) {
    loom_value_facts_mark_lane_varying(&facts);
  } else {
    loom_value_facts_mark_uniform_at_scope(&facts, distribution - 1);
  }
  return facts;
}

void loom_value_fact_control_prepare_diagnostics(
    loom_value_fact_control_t* control) {
  if (control->diagnostics_valid) {
    return;
  }
  IREE_ASSERT(control->worklist_count == 0);
  const loom_cfg_control_t* structure = control->structure;
  for (uint32_t i = structure->component_count; i > 0; --i) {
    loom_value_fact_control_component_t* component =
        &control->components[i - 1];
    const uint8_t distribution = component->distribution;
    loom_cfg_edge_index_t controller = LOOM_CFG_EDGE_INDEX_INVALID;
    if (distribution < LOOM_VALUE_FACT_CONTROL_TOP) {
      // Preserve a still-valid witness so equally weak alternatives do not
      // gratuitously change the diagnostic chosen across analysis updates.
      const loom_cfg_edge_index_t previous = component->controller;
      if (previous != LOOM_CFG_EDGE_INDEX_INVALID &&
          control->blocks[structure->graph->edges[previous].source_block_index]
                  .selector_distribution == distribution) {
        controller = previous;
      } else {
        const loom_cfg_control_input_t* witness =
            &structure->inputs[component->heads[distribution]];
        controller =
            witness->source_component == LOOM_CFG_CONTROL_INVALID
                ? witness->edge
                : control->components[witness->source_component].controller;
      }
    }
    component->controller = controller;
  }
  control->diagnostics_valid = true;
}

loom_cfg_edge_index_t loom_value_fact_control_controller(
    const loom_value_fact_control_t* control, uint16_t block_index) {
  IREE_ASSERT(control->diagnostics_valid);
  const loom_cfg_control_t* structure = control->structure;
  if (!structure->node_count ||
      !structure->graph->blocks[block_index].reachable) {
    return LOOM_CFG_EDGE_INDEX_INVALID;
  }
  return control
      ->components[structure->nodes[structure->blocks[block_index].node]
                       .component]
      .controller;
}

bool loom_value_fact_control_take_changed_block(
    loom_value_fact_control_t* control, uint16_t* out_block_index) {
  while (control->pending_count) {
    const uint16_t index = control->pending_blocks[--control->pending_count];
    loom_value_fact_control_block_t* block = &control->blocks[index];
    block->pending = false;
    const uint8_t distribution =
        control
            ->components[control->structure
                             ->nodes[control->structure->blocks[index].node]
                             .component]
            .distribution;
    if (distribution != block->old_distribution) {
      *out_block_index = index;
      return true;
    }
  }
  return false;
}
