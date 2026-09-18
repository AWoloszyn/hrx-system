// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/type_dependencies.h"

#include <string.h>

#include "iree/base/internal/math.h"
#include "loom/ir/parameterized_type.h"

//===----------------------------------------------------------------------===//
// Stable records and exact canonical indexes
//===----------------------------------------------------------------------===//

enum {
  LOOM_TYPE_DEPENDENCY_SEGMENT_SHIFT = 7,
  LOOM_TYPE_DEPENDENCY_SEGMENT_CAPACITY = 128,
  LOOM_TYPE_DEPENDENCY_SEGMENT_MASK = 127,
};

// One-based records. Both parent edges and radix-map edges tag a record ID's
// low bit, leaving 31 bits for identities. Record addresses never move.
typedef struct loom_dependency_records_t {
  // Published records; prepared capacity is not included.
  uint32_t count;
  // Fixed-size record pages allocated from the dependency arena.
  loom_segmented_storage_t segments;
} loom_dependency_records_t;

typedef struct loom_dependency_node_t {
  // Disjoint lower and upper membership subtrees; both zero for a singleton.
  uint32_t children[2];
  // Smallest member of the set, also its compressed-prefix representative.
  loom_value_id_t provider;
  // Highest differing provider bit, or -1 for a singleton.
  int32_t bit;
  // First active parent edge, encoded as parent ID shifted left plus side.
  uint32_t first_parent;
  // First carrier attached directly to this set.
  uint32_t first_carrier;
  // Number of active parents and directly attached carriers.
  uint32_t active_owners;
  // Links for this node's two edges in its children's active-parent lists.
  struct {
    // Previous active parent edge, or zero at the list head.
    uint32_t previous;
    // Next active parent edge, or zero at the list tail.
    uint32_t next;
  } edges[2];
} loom_dependency_node_t;

typedef struct loom_dependency_carrier_t {
  // Full declared membership, including providers not yet defined.
  uint32_t declared;
  // Currently active membership, or zero while dropped.
  uint32_t active;
  // Prepared membership used by failure-atomic bulk refresh.
  uint32_t prepared;
  // Previous carrier on the active root, or zero at the list head.
  uint32_t previous;
  // Next carrier on the active root, or next recycled carrier while unused.
  uint32_t next;
  // Value whose type carries these dependencies.
  loom_value_id_t value;
} loom_dependency_carrier_t;

typedef struct loom_dependency_union_t {
  // Ordered pair of input set IDs; each pair is evaluated once.
  uint64_t key;
  // Canonical result of the union.
  uint32_t result;
} loom_dependency_union_t;

// A compressed sixteen-way exact map over 64-bit keys has at most 16 levels.
// Leaves name canonical records; branches have the low bit set. Empty is zero.
typedef struct loom_dependency_radix_t {
  // Representative key supplying the compressed low-bit prefix.
  uint64_t key;
  // Tagged edges selected by the key nibble at shift.
  uint32_t children[16];
  // Nibble-aligned bit offset, increasing along a branch path.
  uint32_t shift;
} loom_dependency_radix_t;

struct loom_type_dependency_index_t {
  // Immutable membership with active reverse links.
  loom_dependency_records_t nodes;
  // Value owners, recycled when their declared membership becomes empty.
  loom_dependency_records_t carriers;
  // Memoized composite unions; singleton insertion needs no memo entry.
  loom_dependency_records_t unions;
  // Shared branch storage for both exact maps.
  loom_dependency_records_t branches;
  // Canonical membership map keyed by child pair or singleton provider.
  uint32_t node_root;
  // Composite-union map keyed by ordered input pair.
  uint32_t union_root;
  // First reusable carrier, or zero when none is available.
  uint32_t free_carrier;
};

static void loom_dependency_records_initialize(
    iree_host_size_t record_size, loom_dependency_records_t* records) {
  loom_segmented_storage_initialize(
      record_size * LOOM_TYPE_DEPENDENCY_SEGMENT_CAPACITY,
      iree_alignof(uint64_t), &records->segments);
}

static void* loom_dependency_record(const loom_dependency_records_t* records,
                                    uint32_t id, iree_host_size_t record_size) {
  uint8_t* segment = (uint8_t*)loom_segmented_storage_const_segment(
      &records->segments, (id - 1) >> LOOM_TYPE_DEPENDENCY_SEGMENT_SHIFT);
  return segment + ((id - 1) & LOOM_TYPE_DEPENDENCY_SEGMENT_MASK) * record_size;
}

// Reserves a stable slot without publishing a record. A failed later reserve
// leaves only reusable page capacity, never a half-interned node.
static iree_status_t loom_dependency_records_prepare(
    iree_arena_allocator_t* arena, loom_dependency_records_t* records,
    iree_host_size_t record_size, void** out_record) {
  if (records->count == INT32_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "type dependency index exhausted its 31-bit IDs");
  }
  const uint32_t segment_index =
      records->count >> LOOM_TYPE_DEPENDENCY_SEGMENT_SHIFT;
  if (segment_index == records->segments.segment_count) {
    void* segment = NULL;
    IREE_RETURN_IF_ERROR(
        loom_segmented_storage_append(&records->segments, arena, &segment));
  }
  *out_record =
      loom_dependency_record(records, records->count + 1, record_size);
  return iree_ok_status();
}

static loom_dependency_node_t* loom_dependency_node(
    const loom_type_dependency_index_t* index, uint32_t id) {
  return (loom_dependency_node_t*)loom_dependency_record(
      &index->nodes, id, sizeof(loom_dependency_node_t));
}

static loom_dependency_carrier_t* loom_dependency_carrier(
    const loom_type_dependency_index_t* index, uint32_t id) {
  return (loom_dependency_carrier_t*)loom_dependency_record(
      &index->carriers, id, sizeof(loom_dependency_carrier_t));
}

static loom_dependency_union_t* loom_dependency_union(
    const loom_type_dependency_index_t* index, uint32_t id) {
  return (loom_dependency_union_t*)loom_dependency_record(
      &index->unions, id, sizeof(loom_dependency_union_t));
}

static loom_dependency_radix_t* loom_dependency_radix(
    const loom_type_dependency_index_t* index, uint32_t id) {
  return (loom_dependency_radix_t*)loom_dependency_record(
      &index->branches, id, sizeof(loom_dependency_radix_t));
}

static iree_status_t loom_type_dependencies_allocate_index(
    loom_type_use_table_t* table) {
  if (table->index) {
    return iree_ok_status();
  }
  loom_type_dependency_index_t* index = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(&table->arena, sizeof(*index), (void**)&index));
  memset(index, 0, sizeof(*index));
  loom_dependency_records_initialize(sizeof(loom_dependency_node_t),
                                     &index->nodes);
  loom_dependency_records_initialize(sizeof(loom_dependency_carrier_t),
                                     &index->carriers);
  loom_dependency_records_initialize(sizeof(loom_dependency_union_t),
                                     &index->unions);
  loom_dependency_records_initialize(sizeof(loom_dependency_radix_t),
                                     &index->branches);
  table->index = index;
  return iree_ok_status();
}

typedef enum loom_dependency_key_kind_e {
  LOOM_DEPENDENCY_KEY_NODE,
  LOOM_DEPENDENCY_KEY_UNION,
} loom_dependency_key_kind_t;

static uint64_t loom_dependency_key(const loom_type_dependency_index_t* index,
                                    loom_dependency_key_kind_t kind,
                                    uint32_t id) {
  if (kind == LOOM_DEPENDENCY_KEY_UNION) {
    return loom_dependency_union(index, id)->key;
  }
  const loom_dependency_node_t* node = loom_dependency_node(index, id);
  return node->bit < 0
             ? (uint64_t)node->provider << 32
             : ((uint64_t)node->children[0] << 32) | node->children[1];
}

// A probe result is valid until the next insertion in this map. Stable page
// storage allows intervening capacity growth without invalidating its edge.
typedef struct loom_dependency_position_t {
  // Existing leaf or subtree edge to replace on insertion.
  uint32_t* edge;
  // Matching record, or zero on a miss.
  uint32_t existing;
  // Branch nibble required when replacing a nonempty edge.
  uint32_t shift;
  // New key.
  uint64_t key;
  // Representative key of the replaced edge.
  uint64_t old_key;
} loom_dependency_position_t;

static loom_dependency_position_t loom_dependency_probe(
    loom_type_dependency_index_t* index, loom_dependency_key_kind_t kind,
    uint32_t* root, uint64_t key) {
  uint32_t* edge = root;
  while (*edge & 1) {
    loom_dependency_radix_t* branch = loom_dependency_radix(index, *edge >> 1);
    const uint64_t difference =
        (key ^ branch->key) & ((UINT64_C(1) << branch->shift) - 1);
    if (difference) {
      return (loom_dependency_position_t){
          .edge = edge,
          .shift =
              (uint32_t)iree_math_count_trailing_zeros_u64(difference) & ~3u,
          .key = key,
          .old_key = branch->key,
      };
    }
    edge = &branch->children[(key >> branch->shift) & 15];
  }
  if (!*edge) {
    return (loom_dependency_position_t){.edge = edge, .key = key};
  }
  const uint64_t old_key = loom_dependency_key(index, kind, *edge >> 1);
  if (old_key == key) {
    return (loom_dependency_position_t){.edge = edge, .existing = *edge >> 1};
  }
  return (loom_dependency_position_t){
      .edge = edge,
      .shift =
          (uint32_t)iree_math_count_trailing_zeros_u64(key ^ old_key) & ~3u,
      .key = key,
      .old_key = old_key,
  };
}

static iree_status_t loom_dependency_insert(loom_type_use_table_t* table,
                                            loom_dependency_position_t position,
                                            uint32_t record_id) {
  if (!*position.edge) {
    *position.edge = record_id << 1;
    return iree_ok_status();
  }
  loom_dependency_radix_t* branch = NULL;
  IREE_RETURN_IF_ERROR(
      loom_dependency_records_prepare(&table->arena, &table->index->branches,
                                      sizeof(*branch), (void**)&branch));
  *branch = (loom_dependency_radix_t){
      .key = position.old_key,
      .shift = position.shift,
  };
  branch->children[(position.old_key >> position.shift) & 15] = *position.edge;
  branch->children[(position.key >> position.shift) & 15] = record_id << 1;
  *position.edge = (++table->index->branches.count << 1) | 1;
  return iree_ok_status();
}

static iree_status_t loom_dependency_intern_node(
    loom_type_use_table_t* table, uint64_t key,
    const loom_dependency_node_t* candidate, uint32_t* out_id) {
  loom_dependency_position_t position = loom_dependency_probe(
      table->index, LOOM_DEPENDENCY_KEY_NODE, &table->index->node_root, key);
  if (position.existing) {
    *out_id = position.existing;
    return iree_ok_status();
  }
  loom_dependency_node_t* node = NULL;
  IREE_RETURN_IF_ERROR(loom_dependency_records_prepare(
      &table->arena, &table->index->nodes, sizeof(*node), (void**)&node));
  IREE_RETURN_IF_ERROR(
      loom_dependency_insert(table, position, table->index->nodes.count + 1));
  *node = *candidate;
  *out_id = ++table->index->nodes.count;
  return iree_ok_status();
}

static int32_t loom_dependency_highest_bit(uint32_t value) {
  return 31 - iree_math_count_leading_zeros_u32(value);
}

static iree_status_t loom_dependency_join(loom_type_use_table_t* table,
                                          uint32_t left, uint32_t right,
                                          uint32_t* out_root) {
  if (!left || left == right) {
    *out_root = right;
    return iree_ok_status();
  }
  if (!right) {
    *out_root = left;
    return iree_ok_status();
  }
  const uint32_t left_provider =
      loom_dependency_node(table->index, left)->provider;
  const uint32_t right_provider =
      loom_dependency_node(table->index, right)->provider;
  const int32_t bit =
      loom_dependency_highest_bit(left_provider ^ right_provider);
  // Disjoint children are the uniqueness proof for both directions of query.
  IREE_ASSERT(loom_dependency_node(table->index, left)->bit < bit &&
              loom_dependency_node(table->index, right)->bit < bit);
  if ((left_provider >> bit) & 1) {
    uint32_t temporary = left;
    left = right;
    right = temporary;
  }
  const loom_dependency_node_t candidate = {
      .children = {left, right},
      .provider = loom_dependency_node(table->index, left)->provider,
      .bit = bit,
  };
  return loom_dependency_intern_node(table, ((uint64_t)left << 32) | right,
                                     &candidate, out_root);
}

iree_status_t loom_type_dependencies_union_nonempty(
    loom_type_use_table_t* table, loom_type_dependency_id_t first,
    loom_type_dependency_id_t second, loom_type_dependency_id_t* out_root) {
  if (first > second) {
    uint32_t temporary = first;
    first = second;
    second = temporary;
  }
  const loom_dependency_node_t* a = loom_dependency_node(table->index, first);
  const loom_dependency_node_t* b = loom_dependency_node(table->index, second);
  const uint64_t key = ((uint64_t)first << 32) | second;
  const bool memoize = a->bit >= 0 && b->bit >= 0;
  if (memoize) {
    loom_dependency_position_t position =
        loom_dependency_probe(table->index, LOOM_DEPENDENCY_KEY_UNION,
                              &table->index->union_root, key);
    if (position.existing) {
      *out_root =
          loom_dependency_union(table->index, position.existing)->result;
      return iree_ok_status();
    }
  }
  uint32_t result = 0;
  if (loom_dependency_highest_bit(a->provider ^ b->provider) >
      iree_max(a->bit, b->bit)) {
    IREE_RETURN_IF_ERROR(loom_dependency_join(table, first, second, &result));
  } else if (a->bit == b->bit) {
    uint32_t left = 0;
    uint32_t right = 0;
    IREE_RETURN_IF_ERROR(loom_type_dependencies_union(table, a->children[0],
                                                      b->children[0], &left));
    IREE_RETURN_IF_ERROR(loom_type_dependencies_union(table, a->children[1],
                                                      b->children[1], &right));
    IREE_RETURN_IF_ERROR(loom_dependency_join(table, left, right, &result));
  } else {
    const loom_dependency_node_t* higher = a->bit > b->bit ? a : b;
    const loom_dependency_node_t* lower = a->bit > b->bit ? b : a;
    const uint32_t side = (lower->provider >> higher->bit) & 1;
    uint32_t changed = 0;
    IREE_RETURN_IF_ERROR(loom_type_dependencies_union(
        table, higher->children[side], a->bit > b->bit ? second : first,
        &changed));
    if (changed == higher->children[side]) {
      result = a->bit > b->bit ? first : second;
    } else {
      IREE_RETURN_IF_ERROR(
          loom_dependency_join(table, side ? higher->children[0] : changed,
                               side ? changed : higher->children[1], &result));
    }
  }
  if (memoize) {
    // Recursive unions may have changed this map since the initial miss.
    const loom_dependency_position_t position =
        loom_dependency_probe(table->index, LOOM_DEPENDENCY_KEY_UNION,
                              &table->index->union_root, key);
    loom_dependency_union_t* entry = NULL;
    IREE_RETURN_IF_ERROR(loom_dependency_records_prepare(
        &table->arena, &table->index->unions, sizeof(*entry), (void**)&entry));
    IREE_RETURN_IF_ERROR(loom_dependency_insert(
        table, position, table->index->unions.count + 1));
    *entry = (loom_dependency_union_t){.key = key, .result = result};
    ++table->index->unions.count;
  }
  *out_root = result;
  return iree_ok_status();
}

iree_status_t loom_type_dependencies_add(loom_type_use_table_t* table,
                                         loom_type_dependency_id_t root,
                                         loom_value_id_t provider,
                                         loom_type_dependency_id_t* out_root) {
  IREE_RETURN_IF_ERROR(loom_type_dependencies_allocate_index(table));
  const loom_dependency_node_t candidate = {.provider = provider, .bit = -1};
  uint32_t singleton = 0;
  IREE_RETURN_IF_ERROR(loom_dependency_intern_node(
      table, (uint64_t)provider << 32, &candidate, &singleton));
  return loom_type_dependencies_union(table, root, singleton, out_root);
}

// Intersects membership with [0, limit) along at most one 32-bit boundary path.
static iree_status_t loom_dependency_prefix(loom_type_use_table_t* table,
                                            uint32_t root, uint64_t limit,
                                            uint32_t* out_root) {
  if (!root) {
    *out_root = 0;
    return iree_ok_status();
  }
  const loom_dependency_node_t* node = loom_dependency_node(table->index, root);
  if (node->provider >= limit) {
    *out_root = 0;
    return iree_ok_status();
  }
  if (node->bit < 0) {
    *out_root = root;
    return iree_ok_status();
  }
  const uint64_t mask = (UINT64_C(1) << (node->bit + 1)) - 1;
  if (((uint64_t)node->provider | mask) < limit) {
    *out_root = root;
    return iree_ok_status();
  }
  const uint64_t boundary =
      ((uint64_t)node->provider & ~mask) | (UINT64_C(1) << node->bit);
  if (limit <= boundary) {
    return loom_dependency_prefix(table, node->children[0], limit, out_root);
  }
  uint32_t right = 0;
  IREE_RETURN_IF_ERROR(
      loom_dependency_prefix(table, node->children[1], limit, &right));
  return loom_dependency_join(table, node->children[0], right, out_root);
}

//===----------------------------------------------------------------------===//
// Construction-time dependency facts
//===----------------------------------------------------------------------===//

// Attribute payloads and TYPE IDs have already passed canonical construction.
// Aggregate recursion visits immediate payload ownership, never a child type.
static iree_status_t loom_dependency_collect_attribute(
    loom_module_t* module, const loom_attribute_t* attribute, uint32_t* root) {
  switch ((loom_attr_kind_t)attribute->kind) {
    case LOOM_ATTR_TYPE:
      return loom_type_dependencies_union(
          &module->type_uses, *root,
          module->types.dependencies[attribute->type_id], root);
    case LOOM_ATTR_PREDICATE_LIST:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        const loom_predicate_t* predicate = &attribute->predicate_list[i];
        for (uint8_t j = 0; j < predicate->arg_count; ++j) {
          if (predicate->arg_tags[j] == LOOM_PRED_ARG_VALUE) {
            IREE_RETURN_IF_ERROR(loom_type_dependencies_add(
                &module->type_uses, *root, (loom_value_id_t)predicate->args[j],
                root));
          }
        }
      }
      break;
    case LOOM_ATTR_DICT:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_dependency_collect_attribute(
            module, &attribute->dict_entries[i].value, root));
      }
      break;
    case LOOM_ATTR_PARAMETERIZED:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_dependency_collect_attribute(
            module, &attribute->parameterized_slots[i], root));
      }
      break;
    case LOOM_ATTR_PARAMETERIZED_ARRAY:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_dependency_collect_attribute(
            module, &attribute->parameterized_array[i], root));
      }
      break;
    default:
      break;
  }
  return iree_ok_status();
}

iree_status_t loom_type_dependencies_collect_immediate(
    loom_module_t* module, loom_type_t type,
    loom_type_dependency_id_t* out_root) {
  if (!loom_type_kind_is_valid(loom_type_kind(type)) ||
      !loom_type_may_reference_values(type)) {
    return iree_ok_status();
  }
  switch (loom_type_kind(type)) {
    case LOOM_TYPE_PARAMETERIZED: {
      const loom_attribute_t* parameters =
          loom_type_parameterized_parameters(type);
      const uint8_t count = loom_type_parameterized_parameter_count(type);
      for (uint8_t i = 0; i < count; ++i) {
        IREE_RETURN_IF_ERROR(loom_dependency_collect_attribute(
            module, &parameters[i], out_root));
      }
      return iree_ok_status();
    }
    default:
      break;
  }
  if (loom_type_is_shaped(type) || loom_type_is_pool(type)) {
    for (uint8_t i = 0; i < loom_type_rank(type); ++i) {
      if (loom_type_dim_is_dynamic_at(type, i)) {
        IREE_RETURN_IF_ERROR(loom_type_dependencies_add(
            &module->type_uses, *out_root, loom_type_dim_value_id_at(type, i),
            out_root));
      }
    }
  }
  if (loom_type_has_ssa_encoding(type)) {
    IREE_RETURN_IF_ERROR(loom_type_dependencies_add(
        &module->type_uses, *out_root, loom_type_encoding_value_id(type),
        out_root));
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Active reverse ownership
//===----------------------------------------------------------------------===//

static void loom_dependency_acquire(loom_type_use_table_t* table, uint32_t id) {
  loom_dependency_node_t* node = loom_dependency_node(table->index, id);
  if (node->active_owners++) {
    return;
  }
  if (node->bit < 0) {
    loom_value_table_type_use_heads(table->value_table, node->provider)
        ->provider = id;
    return;
  }
  for (uint32_t side = 0; side < 2; ++side) {
    loom_dependency_node_t* child =
        loom_dependency_node(table->index, node->children[side]);
    const uint32_t edge = (id << 1) | side;
    node->edges[side].previous = 0;
    node->edges[side].next = child->first_parent;
    if (child->first_parent) {
      loom_dependency_node(table->index, child->first_parent >> 1)
          ->edges[child->first_parent & 1]
          .previous = edge;
    }
    child->first_parent = edge;
    loom_dependency_acquire(table, node->children[side]);
  }
}

static void loom_dependency_release(loom_type_use_table_t* table, uint32_t id) {
  loom_dependency_node_t* node = loom_dependency_node(table->index, id);
  if (--node->active_owners || node->bit < 0) {
    return;
  }
  for (uint32_t side = 0; side < 2; ++side) {
    const uint32_t previous = node->edges[side].previous;
    const uint32_t next = node->edges[side].next;
    if (previous) {
      loom_dependency_node(table->index, previous >> 1)
          ->edges[previous & 1]
          .next = next;
    } else {
      loom_dependency_node(table->index, node->children[side])->first_parent =
          next;
    }
    if (next) {
      loom_dependency_node(table->index, next >> 1)->edges[next & 1].previous =
          previous;
    }
    loom_dependency_release(table, node->children[side]);
  }
}

static void loom_dependency_attach(loom_type_use_table_t* table,
                                   uint32_t carrier_id, uint32_t root) {
  loom_dependency_carrier_t* carrier =
      loom_dependency_carrier(table->index, carrier_id);
  if (carrier->active == root) {
    return;
  }
  // Acquiring first keeps shared subtrees active throughout replacement.
  if (root) {
    loom_dependency_acquire(table, root);
    ++table->active_carrier_count;
  }
  if (carrier->active) {
    if (carrier->previous) {
      loom_dependency_carrier(table->index, carrier->previous)->next =
          carrier->next;
    } else {
      loom_dependency_node(table->index, carrier->active)->first_carrier =
          carrier->next;
    }
    if (carrier->next) {
      loom_dependency_carrier(table->index, carrier->next)->previous =
          carrier->previous;
    }
    loom_dependency_release(table, carrier->active);
    --table->active_carrier_count;
  }
  carrier->active = root;
  carrier->previous = 0;
  carrier->next =
      root ? loom_dependency_node(table->index, root)->first_carrier : 0;
  if (carrier->next) {
    loom_dependency_carrier(table->index, carrier->next)->previous = carrier_id;
  }
  if (root) {
    loom_dependency_node(table->index, root)->first_carrier = carrier_id;
  }
}

iree_status_t loom_type_dependencies_prepare(
    loom_type_use_table_t* table, loom_value_id_t value_id,
    loom_type_dependency_id_t root, iree_host_size_t available_value_count,
    loom_type_dependency_assignment_t* out_assignment) {
  *out_assignment = (loom_type_dependency_assignment_t){.declared = root};
  if (!root) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_dependency_prefix(
      table, root, available_value_count, &out_assignment->active));
  uint32_t carrier_id =
      value_id < table->value_table->count
          ? loom_value_table_type_use_heads(table->value_table, value_id)
                ->carrier
          : 0;
  if (!carrier_id) {
    loom_dependency_carrier_t* carrier = NULL;
    if (table->index->free_carrier) {
      carrier_id = table->index->free_carrier;
      carrier = loom_dependency_carrier(table->index, carrier_id);
      table->index->free_carrier = carrier->next;
    } else {
      IREE_RETURN_IF_ERROR(loom_dependency_records_prepare(
          &table->arena, &table->index->carriers, sizeof(*carrier),
          (void**)&carrier));
      carrier_id = ++table->index->carriers.count;
    }
    *carrier = (loom_dependency_carrier_t){.value = value_id};
  }
  out_assignment->carrier = carrier_id;
  return iree_ok_status();
}

void loom_type_dependencies_commit(
    loom_type_use_table_t* table, loom_value_id_t value_id,
    const loom_type_dependency_assignment_t* assignment) {
  loom_value_type_use_heads_t* heads =
      loom_value_table_type_use_heads(table->value_table, value_id);
  if (assignment->carrier) {
    loom_dependency_attach(table, assignment->carrier, assignment->active);
    loom_dependency_carrier(table->index, assignment->carrier)->declared =
        assignment->declared;
    heads->carrier = assignment->carrier;
  } else if (heads->carrier) {
    loom_dependency_attach(table, heads->carrier, 0);
    loom_dependency_carrier_t* carrier =
        loom_dependency_carrier(table->index, heads->carrier);
    carrier->declared = 0;
    carrier->next = table->index->free_carrier;
    table->index->free_carrier = heads->carrier;
    heads->carrier = 0;
  }
}

iree_status_t loom_type_dependencies_refresh(loom_type_use_table_t* table,
                                             loom_value_id_t value_id) {
  const uint32_t carrier_id =
      loom_value_table_type_use_heads(table->value_table, value_id)->carrier;
  if (!carrier_id) {
    return iree_ok_status();
  }
  loom_dependency_carrier_t* carrier =
      loom_dependency_carrier(table->index, carrier_id);
  uint32_t root = 0;
  IREE_RETURN_IF_ERROR(loom_dependency_prefix(
      table, carrier->declared, table->value_table->count, &root));
  loom_dependency_attach(table, carrier_id, root);
  return iree_ok_status();
}

static bool loom_dependency_carrier_is_live(const loom_value_t* value) {
  if (loom_value_is_block_arg(value)) {
    return true;
  }
  const loom_op_t* defining_op = loom_value_def_op(value);
  if (defining_op) {
    return !iree_any_bit_set(defining_op->flags, LOOM_OP_FLAG_DEAD);
  }
  return value->use_count != 0;
}

iree_status_t loom_type_dependencies_recompute(loom_type_use_table_t* table) {
  if (!table->index) {
    return iree_ok_status();
  }
  for (uint32_t id = 1; id <= table->index->carriers.count; ++id) {
    loom_dependency_carrier_t* carrier =
        loom_dependency_carrier(table->index, id);
    carrier->prepared = 0;
    if (carrier->declared &&
        loom_dependency_carrier_is_live(
            loom_value_table_const_value(table->value_table, carrier->value))) {
      IREE_RETURN_IF_ERROR(loom_dependency_prefix(table, carrier->declared,
                                                  table->value_table->count,
                                                  &carrier->prepared));
    }
  }
  for (uint32_t id = 1; id <= table->index->carriers.count; ++id) {
    loom_dependency_carrier_t* carrier =
        loom_dependency_carrier(table->index, id);
    if (carrier->declared) {
      loom_dependency_attach(table, id, carrier->prepared);
    }
  }
  return iree_ok_status();
}

void loom_type_dependencies_drop(loom_type_use_table_t* table,
                                 loom_value_id_t value_id) {
  if (value_id >= table->value_table->count) {
    return;
  }
  const uint32_t carrier =
      loom_value_table_type_use_heads(table->value_table, value_id)->carrier;
  if (carrier) {
    loom_dependency_attach(table, carrier, 0);
  }
}

bool loom_type_dependencies_has_users(const loom_type_use_table_t* table,
                                      loom_value_id_t value_id) {
  if (value_id >= table->value_table->count) {
    return false;
  }
  const uint32_t provider =
      loom_value_table_const_type_use_heads(table->value_table, value_id)
          ->provider;
  return provider &&
         loom_dependency_node(table->index, provider)->active_owners != 0;
}

//===----------------------------------------------------------------------===//
// Bounded membership and ownership cursors
//===----------------------------------------------------------------------===//

void loom_type_dependencies_begin(const loom_type_use_table_t* table,
                                  loom_value_id_t value_id,
                                  loom_type_use_iterator_t* out_iterator) {
  out_iterator->index = table->index;
  out_iterator->pending_count = 0;
  out_iterator->carrier = 0;
  if (value_id < table->value_table->count) {
    const uint32_t carrier =
        loom_value_table_const_type_use_heads(table->value_table, value_id)
            ->carrier;
    const uint32_t root =
        carrier ? loom_dependency_carrier(table->index, carrier)->active : 0;
    if (root) {
      out_iterator->pending[out_iterator->pending_count++] = root;
    }
  }
}

loom_value_id_t loom_type_dependencies_next(
    loom_type_use_iterator_t* iterator) {
  while (iterator->pending_count) {
    const loom_dependency_node_t* node = loom_dependency_node(
        iterator->index, iterator->pending[--iterator->pending_count]);
    if (node->bit < 0) {
      return node->provider;
    }
    iterator->pending[iterator->pending_count++] = node->children[1];
    iterator->pending[iterator->pending_count++] = node->children[0];
  }
  return LOOM_VALUE_ID_INVALID;
}

void loom_type_users_begin(const loom_type_use_table_t* table,
                           loom_value_id_t value_id,
                           loom_type_use_iterator_t* out_iterator) {
  out_iterator->index = table->index;
  out_iterator->pending_count = 0;
  out_iterator->carrier = 0;
  if (loom_type_dependencies_has_users(table, value_id)) {
    const uint32_t provider =
        loom_value_table_const_type_use_heads(table->value_table, value_id)
            ->provider;
    const loom_dependency_node_t* node =
        loom_dependency_node(table->index, provider);
    out_iterator->carrier = node->first_carrier;
    if (node->first_parent) {
      out_iterator->pending[out_iterator->pending_count++] = node->first_parent;
    }
  }
}

loom_value_id_t loom_type_users_next(loom_type_use_iterator_t* iterator) {
  while (!iterator->carrier && iterator->pending_count) {
    const uint32_t edge = iterator->pending[iterator->pending_count - 1];
    const loom_dependency_node_t* node =
        loom_dependency_node(iterator->index, edge >> 1);
    const uint32_t next = node->edges[edge & 1].next;
    if (next) {
      iterator->pending[iterator->pending_count - 1] = next;
    } else {
      --iterator->pending_count;
    }
    iterator->carrier = node->first_carrier;
    if (node->first_parent) {
      iterator->pending[iterator->pending_count++] = node->first_parent;
    }
  }
  if (!iterator->carrier) {
    return LOOM_VALUE_ID_INVALID;
  }
  const loom_dependency_carrier_t* carrier =
      loom_dependency_carrier(iterator->index, iterator->carrier);
  iterator->carrier = carrier->next;
  return carrier->value;
}
