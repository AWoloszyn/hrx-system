// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/atomic_ordering.h"

#include "loom/ops/atomic.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/memory.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/planning/wait_packets.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"
#include "loom/target/arch/amdgpu/target_info.h"

typedef uint8_t loom_amdgpu_atomic_global_ordering_flags_t;

// The target memory model has a global atomic ordering rule.
#define LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SUPPORTED ((uint8_t)1u << 0)
// Atomic packets using this model require an explicit device scope attr.
#define LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_PACKET_SCOPE_DEVICE \
  ((uint8_t)1u << 1)
// Integer system updates share this model's device ordering recipe.
#define LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SYSTEM_INTEGER ((uint8_t)1u << 2)

typedef struct loom_amdgpu_atomic_global_ordering_rule_t {
  // Rule availability and packet-attribute behavior.
  loom_amdgpu_atomic_global_ordering_flags_t flags;
  // Wait-counter masks emitted before release atomics.
  uint32_t release_wait_masks[LOOM_AMDGPU_ATOMIC_WAIT_CAPACITY];
  // Number of populated release wait-counter masks.
  uint8_t release_wait_count;
  // Scoped cache writeback emitted after the release waits, or zero.
  loom_amdgpu_descriptor_ref_t release_writeback;
  // Completion counter for the scoped writeback, or zero.
  uint32_t release_writeback_wait_mask;
  // Wait-counter masks emitted after acquire atomics by operation kind.
  uint32_t acquire_wait_masks[LOOM_AMDGPU_ATOMIC_OPERATION_COUNT_];
  // Additional completion domain for a flat atomic that may resolve to LDS.
  uint32_t flat_acquire_wait_mask;
  // Cache-control packets emitted after acquire atomics.
  loom_amdgpu_atomic_explicit_packet_selection_t
      acquire_cache_controls[LOOM_AMDGPU_ATOMIC_VISIBILITY_CAPACITY];
  // Number of populated acquire cache-control packets.
  uint8_t acquire_cache_control_count;
  // Completion required after cache invalidation, or zero.
  uint32_t acquire_cache_control_wait_mask;
  // Packet scope immediate/attribute value when required by |flags|.
  uint8_t packet_scope;
} loom_amdgpu_atomic_global_ordering_rule_t;

// Device scope value encoded by VGLOBAL SCOPE immediates.
#define LOOM_AMDGPU_GLOBAL_SCOPE_DEVICE 2

static const loom_amdgpu_atomic_global_ordering_rule_t
    kAmdgpuAtomicGlobalOrderingRules[LOOM_AMDGPU_MEMORY_ORDERING_MODEL_GFX125 +
                                     1] = {
        [LOOM_AMDGPU_MEMORY_ORDERING_MODEL_GFX11] =
            {
                .flags = LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SUPPORTED |
                         LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SYSTEM_INTEGER,
                .release_wait_masks =
                    {
                        // A release orders preceding accesses through all
                        // address spaces, including workgroup storage.
                        LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD |
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS |
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_SMEM,
                        LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_STORE,
                    },
                .release_wait_count = 2,
                .acquire_wait_masks =
                    {
                        [LOOM_AMDGPU_ATOMIC_OPERATION_REDUCE] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_STORE,
                        [LOOM_AMDGPU_ATOMIC_OPERATION_RMW] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                        [LOOM_AMDGPU_ATOMIC_OPERATION_CMPXCHG] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                    },
                .flat_acquire_wait_mask = LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS |
                                          LOOM_AMDGPU_WAIT_COUNTER_MASK_SMEM,
                .acquire_cache_controls =
                    {
                        {
                            .descriptor_ref =
                                LOOM_AMDGPU_DESCRIPTOR_REF_BUFFER_GL1_INV,
                        },
                        {
                            .descriptor_ref =
                                LOOM_AMDGPU_DESCRIPTOR_REF_BUFFER_GL0_INV,
                        },
                    },
                .acquire_cache_control_count = 2,
            },
        [LOOM_AMDGPU_MEMORY_ORDERING_MODEL_GFX12] =
            {
                .flags = LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SUPPORTED |
                         LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_PACKET_SCOPE_DEVICE,
                // The shared memory frontier orders preceding global accesses.
                // A global publication also releases the separate LDS domain.
                .release_wait_masks = {LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS},
                .release_wait_count = 1,
                .flat_acquire_wait_mask = LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS,
                .acquire_wait_masks =
                    {
                        [LOOM_AMDGPU_ATOMIC_OPERATION_REDUCE] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_STORE,
                        [LOOM_AMDGPU_ATOMIC_OPERATION_RMW] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                        [LOOM_AMDGPU_ATOMIC_OPERATION_CMPXCHG] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                    },
                .acquire_cache_controls =
                    {
                        {
                            .descriptor_ref =
                                LOOM_AMDGPU_DESCRIPTOR_REF_GLOBAL_INV,
                            .immediates =
                                {
                                    {
                                        .name = IREE_SVL("scope"),
                                        .value =
                                            LOOM_AMDGPU_GLOBAL_SCOPE_DEVICE,
                                    },
                                },
                            .immediate_count = 1,
                        },
                    },
                .acquire_cache_control_count = 1,
                .packet_scope = LOOM_AMDGPU_GLOBAL_SCOPE_DEVICE,
            },
        [LOOM_AMDGPU_MEMORY_ORDERING_MODEL_GFX125] =
            {
                .flags = LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SUPPORTED |
                         LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_PACKET_SCOPE_DEVICE,
                // The shared memory frontier orders preceding global accesses.
                // A global publication also releases the separate LDS domain.
                .release_wait_masks = {LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS},
                .release_wait_count = 1,
                .release_writeback = LOOM_AMDGPU_DESCRIPTOR_REF_GLOBAL_WB,
                .release_writeback_wait_mask =
                    LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_STORE,
                .acquire_cache_control_wait_mask =
                    LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                .flat_acquire_wait_mask = LOOM_AMDGPU_WAIT_COUNTER_MASK_LDS,
                .acquire_wait_masks =
                    {
                        [LOOM_AMDGPU_ATOMIC_OPERATION_REDUCE] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_STORE,
                        [LOOM_AMDGPU_ATOMIC_OPERATION_RMW] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                        [LOOM_AMDGPU_ATOMIC_OPERATION_CMPXCHG] =
                            LOOM_AMDGPU_WAIT_COUNTER_MASK_VMEM_LOAD,
                    },
                .acquire_cache_controls =
                    {
                        {
                            .descriptor_ref =
                                LOOM_AMDGPU_DESCRIPTOR_REF_GLOBAL_INV,
                            .immediates =
                                {
                                    {
                                        .name = IREE_SVL("scope"),
                                        .value =
                                            LOOM_AMDGPU_GLOBAL_SCOPE_DEVICE,
                                    },
                                },
                            .immediate_count = 1,
                        },
                    },
                .acquire_cache_control_count = 1,
                .packet_scope = LOOM_AMDGPU_GLOBAL_SCOPE_DEVICE,
            },
};

static const loom_amdgpu_atomic_global_ordering_rule_t*
loom_amdgpu_atomic_global_ordering_rule_lookup(
    const loom_low_descriptor_set_t* descriptor_set) {
  const loom_amdgpu_descriptor_set_info_t* info =
      loom_amdgpu_target_info_descriptor_set_at(
          descriptor_set->descriptor_set_ordinal);
  const loom_amdgpu_atomic_global_ordering_rule_t* rule =
      &kAmdgpuAtomicGlobalOrderingRules[info->vector_memory.ordering_model];
  return iree_any_bit_set(rule->flags,
                          LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SUPPORTED)
             ? rule
             : NULL;
}

static bool loom_amdgpu_atomic_ordering_has_acquire(uint8_t ordering) {
  switch (ordering) {
    case LOOM_ATOMIC_ORDERING_ACQUIRE:
    case LOOM_ATOMIC_ORDERING_ACQ_REL:
    case LOOM_ATOMIC_ORDERING_SEQ_CST:
      return true;
    default:
      return false;
  }
}

static bool loom_amdgpu_atomic_ordering_has_release(uint8_t ordering) {
  switch (ordering) {
    case LOOM_ATOMIC_ORDERING_RELEASE:
    case LOOM_ATOMIC_ORDERING_ACQ_REL:
    case LOOM_ATOMIC_ORDERING_SEQ_CST:
      return true;
    default:
      return false;
  }
}

static bool loom_amdgpu_atomic_source_has_acquire_ordering(
    const loom_low_source_memory_access_plan_t* source) {
  return loom_amdgpu_atomic_ordering_has_acquire(source->atomic.ordering) ||
         loom_amdgpu_atomic_ordering_has_acquire(
             source->atomic.failure_ordering);
}

static bool loom_amdgpu_atomic_source_has_release_ordering(
    const loom_low_source_memory_access_plan_t* source) {
  return loom_amdgpu_atomic_ordering_has_release(source->atomic.ordering);
}

static bool loom_amdgpu_atomic_global_ordering_supported(
    const loom_low_descriptor_set_t* descriptor_set, uint8_t ordering) {
  if (ordering != LOOM_ATOMIC_ORDERING_ACQUIRE &&
      ordering != LOOM_ATOMIC_ORDERING_RELEASE &&
      ordering != LOOM_ATOMIC_ORDERING_ACQ_REL &&
      ordering != LOOM_ATOMIC_ORDERING_SEQ_CST) {
    return false;
  }
  return loom_amdgpu_atomic_global_ordering_rule_lookup(descriptor_set) != NULL;
}

static bool loom_amdgpu_atomic_memory_space_is_device_visible(
    loom_value_fact_memory_space_t memory_space) {
  return memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL ||
         memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_GENERIC;
}

bool loom_amdgpu_atomic_scope_supported(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source,
    loom_type_t value_type) {
  if (source->memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
    return source->atomic.scope == LOOM_ATOMIC_SCOPE_WORKGROUP;
  }
  if (!loom_amdgpu_atomic_memory_space_is_device_visible(
          source->memory_space)) {
    return false;
  }
  if (source->atomic.scope == LOOM_ATOMIC_SCOPE_DEVICE) {
    return true;
  }
  if (source->atomic.scope != LOOM_ATOMIC_SCOPE_SYSTEM ||
      (!loom_amdgpu_type_is_i32(value_type) &&
       !loom_amdgpu_type_is_i64(value_type))) {
    return false;
  }
  // GFX11 integer atomics reach the mapping's coherence point without a scope
  // immediate. Other cache hierarchies require their own system recipe.
  const loom_amdgpu_atomic_global_ordering_rule_t* rule =
      loom_amdgpu_atomic_global_ordering_rule_lookup(descriptor_set);
  return rule &&
         iree_any_bit_set(rule->flags,
                          LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_SYSTEM_INTEGER);
}

static bool loom_amdgpu_atomic_ordering_supported(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_value_fact_memory_space_t memory_space, uint8_t ordering) {
  if (ordering == LOOM_ATOMIC_ORDERING_RELAXED) {
    return true;
  }
  if (loom_amdgpu_atomic_memory_space_is_device_visible(memory_space)) {
    return loom_amdgpu_atomic_global_ordering_supported(descriptor_set,
                                                        ordering);
  }
  if (memory_space != LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP) {
    return false;
  }
  return ordering == LOOM_ATOMIC_ORDERING_ACQUIRE ||
         ordering == LOOM_ATOMIC_ORDERING_RELEASE ||
         ordering == LOOM_ATOMIC_ORDERING_ACQ_REL ||
         ordering == LOOM_ATOMIC_ORDERING_SEQ_CST;
}

bool loom_amdgpu_atomic_orderings_supported(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source) {
  return loom_amdgpu_atomic_ordering_supported(
             descriptor_set, source->memory_space, source->atomic.ordering) &&
         loom_amdgpu_atomic_ordering_supported(descriptor_set,
                                               source->memory_space,
                                               source->atomic.failure_ordering);
}

static bool loom_amdgpu_atomic_append_wait_counter_mask(
    const loom_low_descriptor_set_t* descriptor_set, uint32_t counter_mask,
    loom_amdgpu_atomic_explicit_packet_selection_t* waits,
    iree_host_size_t wait_capacity, iree_host_size_t* inout_wait_count) {
  loom_amdgpu_wait_packet_selection_t selection = {0};
  if (!loom_amdgpu_wait_packet_try_select_counter_mask(
          descriptor_set, counter_mask, /*target_count=*/0, &selection)) {
    return false;
  }
  IREE_ASSERT(*inout_wait_count < wait_capacity);
  IREE_ASSERT(selection.immediate_count <=
              LOOM_AMDGPU_EXPLICIT_PACKET_IMMEDIATE_CAPACITY);
  loom_amdgpu_atomic_explicit_packet_selection_t* wait =
      &waits[(*inout_wait_count)++];
  *wait = (loom_amdgpu_atomic_explicit_packet_selection_t){
      .descriptor_ref = selection.descriptor_ref,
      .immediate_count = selection.immediate_count,
  };
  for (iree_host_size_t i = 0; i < selection.immediate_count; ++i) {
    wait->immediates[i] = (loom_amdgpu_explicit_packet_immediate_template_t){
        .name = selection.immediates[i].name,
        .value = selection.immediates[i].value,
    };
  }
  return true;
}

static bool loom_amdgpu_atomic_select_global_release_packets(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_atomic_global_ordering_rule_t* rule,
    loom_amdgpu_atomic_ordering_selection_t* ordering) {
  for (iree_host_size_t i = 0; i < rule->release_wait_count; ++i) {
    if (!loom_amdgpu_atomic_append_wait_counter_mask(
            descriptor_set, rule->release_wait_masks[i],
            ordering->pre_atomic_packets,
            IREE_ARRAYSIZE(ordering->pre_atomic_packets),
            &ordering->pre_atomic_packet_count)) {
      return false;
    }
  }
  if (rule->release_writeback != 0) {
    ordering->pre_atomic_packets[ordering->pre_atomic_packet_count++] =
        (loom_amdgpu_atomic_explicit_packet_selection_t){
            .descriptor_ref = rule->release_writeback,
            .immediates = {{.name = IREE_SVL("scope"),
                            .value = rule->packet_scope}},
            .immediate_count = 1,
        };
    return loom_amdgpu_atomic_append_wait_counter_mask(
        descriptor_set, rule->release_writeback_wait_mask,
        ordering->pre_atomic_packets,
        IREE_ARRAYSIZE(ordering->pre_atomic_packets),
        &ordering->pre_atomic_packet_count);
  }
  return true;
}

static bool loom_amdgpu_atomic_select_global_acquire_waits(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_atomic_global_ordering_rule_t* rule,
    loom_amdgpu_atomic_ordering_selection_t* ordering,
    loom_amdgpu_atomic_operation_kind_t operation_kind) {
  const uint32_t counter_mask = rule->acquire_wait_masks[operation_kind];
  return loom_amdgpu_atomic_append_wait_counter_mask(
      descriptor_set, counter_mask, ordering->post_atomic_waits,
      IREE_ARRAYSIZE(ordering->post_atomic_waits),
      &ordering->post_atomic_wait_count);
}

static bool loom_amdgpu_atomic_select_global_acquire_cache_controls(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_atomic_global_ordering_rule_t* rule,
    loom_amdgpu_atomic_ordering_selection_t* ordering) {
  for (iree_host_size_t i = 0; i < rule->acquire_cache_control_count; ++i) {
    const loom_amdgpu_atomic_explicit_packet_selection_t* packet =
        &rule->acquire_cache_controls[i];
    if (!loom_amdgpu_descriptor_set_has_ref(descriptor_set,
                                            packet->descriptor_ref)) {
      return false;
    }
    ordering->post_atomic_visibility_packets[i] = *packet;
  }
  ordering->post_atomic_visibility_packet_count =
      rule->acquire_cache_control_count;
  if (rule->acquire_cache_control_wait_mask != 0) {
    return loom_amdgpu_atomic_append_wait_counter_mask(
        descriptor_set, rule->acquire_cache_control_wait_mask,
        ordering->post_atomic_visibility_packets,
        IREE_ARRAYSIZE(ordering->post_atomic_visibility_packets),
        &ordering->post_atomic_visibility_packet_count);
  }
  return true;
}

bool loom_amdgpu_atomic_select_ordering(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source,
    loom_amdgpu_atomic_operation_kind_t operation_kind,
    loom_amdgpu_atomic_ordering_selection_t* ordering) {
  *ordering = (loom_amdgpu_atomic_ordering_selection_t){0};
  const loom_amdgpu_atomic_global_ordering_rule_t* rule =
      loom_amdgpu_atomic_global_ordering_rule_lookup(descriptor_set);
  if (!loom_amdgpu_atomic_memory_space_is_device_visible(
          source->memory_space) ||
      (!loom_amdgpu_atomic_source_has_release_ordering(source) &&
       !loom_amdgpu_atomic_source_has_acquire_ordering(source))) {
    return true;
  }
  if (rule == NULL) {
    return false;
  }

  if (loom_amdgpu_atomic_source_has_release_ordering(source)) {
    if (!loom_amdgpu_atomic_select_global_release_packets(descriptor_set, rule,
                                                          ordering)) {
      return false;
    }
  }
  if (loom_amdgpu_atomic_source_has_acquire_ordering(source)) {
    if (!loom_amdgpu_atomic_select_global_acquire_waits(
            descriptor_set, rule, ordering, operation_kind)) {
      return false;
    }
    if (source->memory_space == LOOM_VALUE_FACT_MEMORY_SPACE_GENERIC &&
        rule->flat_acquire_wait_mask != 0 &&
        !loom_amdgpu_atomic_append_wait_counter_mask(
            descriptor_set, rule->flat_acquire_wait_mask,
            ordering->post_atomic_waits,
            IREE_ARRAYSIZE(ordering->post_atomic_waits),
            &ordering->post_atomic_wait_count)) {
      return false;
    }
    if (!loom_amdgpu_atomic_select_global_acquire_cache_controls(
            descriptor_set, rule, ordering)) {
      return false;
    }
  }
  return true;
}

void loom_amdgpu_atomic_select_packet_attrs(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_source_memory_access_plan_t* source,
    loom_amdgpu_atomic_packet_attrs_t* packet_attrs) {
  *packet_attrs = (loom_amdgpu_atomic_packet_attrs_t){0};
  packet_attrs->scope_attr_name_id = LOOM_STRING_ID_INVALID;
  if (!loom_amdgpu_atomic_memory_space_is_device_visible(
          source->memory_space)) {
    return;
  }
  const loom_amdgpu_atomic_global_ordering_rule_t* rule =
      loom_amdgpu_atomic_global_ordering_rule_lookup(descriptor_set);
  if (rule == NULL ||
      !iree_any_bit_set(
          rule->flags,
          LOOM_AMDGPU_ATOMIC_GLOBAL_ORDERING_PACKET_SCOPE_DEVICE)) {
    return;
  }
  packet_attrs->flags |= LOOM_AMDGPU_ATOMIC_PACKET_ATTR_SCOPE;
  packet_attrs->scope = rule->packet_scope;
}

static iree_status_t loom_amdgpu_atomic_resolve_explicit_packet_selection(
    loom_low_lower_context_t* context,
    const loom_amdgpu_atomic_explicit_packet_selection_t* selection,
    loom_amdgpu_explicit_packet_plan_t* out_plan) {
  bool present = false;
  IREE_RETURN_IF_ERROR(loom_amdgpu_resolve_explicit_packet_plan(
      context, selection->descriptor_ref, selection->immediates,
      selection->immediate_count, out_plan, &present));
  if (!present) {
    IREE_ASSERT_UNREACHABLE(
        "selected AMDGPU explicit atomic ordering packet descriptor");
    IREE_BUILTIN_UNREACHABLE();
  }
  return iree_ok_status();
}

iree_status_t loom_amdgpu_atomic_resolve_ordering_selection(
    loom_low_lower_context_t* context,
    const loom_amdgpu_atomic_ordering_selection_t* selection,
    loom_amdgpu_atomic_ordering_plan_t* out_plan) {
  *out_plan = (loom_amdgpu_atomic_ordering_plan_t){0};
  out_plan->pre_atomic_packet_count = selection->pre_atomic_packet_count;
  for (iree_host_size_t i = 0; i < selection->pre_atomic_packet_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_atomic_resolve_explicit_packet_selection(
        context, &selection->pre_atomic_packets[i],
        &out_plan->pre_atomic_packets[i]));
  }
  out_plan->post_atomic_wait_count = selection->post_atomic_wait_count;
  for (iree_host_size_t i = 0; i < selection->post_atomic_wait_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_atomic_resolve_explicit_packet_selection(
        context, &selection->post_atomic_waits[i],
        &out_plan->post_atomic_waits[i]));
  }
  out_plan->post_atomic_visibility_packet_count =
      selection->post_atomic_visibility_packet_count;
  for (iree_host_size_t i = 0;
       i < selection->post_atomic_visibility_packet_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_atomic_resolve_explicit_packet_selection(
        context, &selection->post_atomic_visibility_packets[i],
        &out_plan->post_atomic_visibility_packets[i]));
  }
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_emit_atomic_ordering_packets(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_explicit_packet_plan_t* packets,
    iree_host_size_t packet_count) {
  for (iree_host_size_t i = 0; i < packet_count; ++i) {
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_emit_explicit_packet_plan(context, source_op, &packets[i]));
  }
  return iree_ok_status();
}

iree_status_t loom_amdgpu_emit_atomic_pre_ordering(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_atomic_ordering_plan_t* ordering) {
  return loom_amdgpu_emit_atomic_ordering_packets(
      context, source_op, ordering->pre_atomic_packets,
      ordering->pre_atomic_packet_count);
}

iree_status_t loom_amdgpu_emit_atomic_post_ordering(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_atomic_ordering_plan_t* ordering) {
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_atomic_ordering_packets(
      context, source_op, ordering->post_atomic_waits,
      ordering->post_atomic_wait_count));
  return loom_amdgpu_emit_atomic_ordering_packets(
      context, source_op, ordering->post_atomic_visibility_packets,
      ordering->post_atomic_visibility_packet_count);
}
