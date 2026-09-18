// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_TYPE_IDENTITY_H_
#define LOOM_IR_TYPE_IDENTITY_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LOOM_TYPE_IDENTITY_PAGE_SHIFT = 12,
  LOOM_TYPE_IDENTITY_PAGE_SIZE = 4096,
  LOOM_TYPE_IDENTITY_PAGE_MASK = 4095,
};

// Canonical payload starts in one address page. Construction accesses the most
// recent page inline; the complete directory owns all older pages.
typedef struct loom_type_identity_page_t {
  // Address page number, independent of arena block allocation boundaries.
  uint64_t key;
  // Canonical ID plus one at each payload start; zero marks unregistered bytes.
  uint32_t ordinals[LOOM_TYPE_IDENTITY_PAGE_SIZE / iree_max_align_t];
} loom_type_identity_page_t;

static inline uint64_t loom_type_identity_key(loom_type_t type) {
  switch (loom_type_kind(type)) {
    case LOOM_TYPE_FUNCTION:
    case LOOM_TYPE_DIALECT:
    case LOOM_TYPE_PARAMETERIZED:
      return type.dims[0];
    case LOOM_TYPE_REGISTER:
      return loom_type_register_has_value_type(type) ? type.dims[0] : 0;
    default:
      return !loom_type_has_inline_dims(type) && loom_type_rank(type) > 0
                 ? type.dims[0]
                 : 0;
  }
}

// Returns the canonical ID for an exact copy of a module-owned pointer-backed
// type, or LOOM_TYPE_ID_INVALID. Inline types use the structural interner.
// Only successful canonical publication registers payloads; caller-owned
// pointers are never retained. Lookup is bounded by the pointer bit width and
// resolves payload starts directly within their address page.
loom_type_id_t loom_type_identity_find(const loom_module_t* module,
                                       loom_type_t type);

// Prepared publication into a stable, arena-owned canonical-payload index.
// Discarding this record leaves the index unchanged.
typedef struct loom_type_identity_insertion_t {
  // Stable directory edge to publish, or NULL when no new page is added.
  uint64_t* edge;
  // Prepared page or branch link.
  uint64_t replacement;
  // Slot for the new canonical type ordinal, or NULL for an inline type.
  uint32_t* ordinal;
  // Owning page, published as the sequential-construction accelerator.
  struct loom_type_identity_page_t* page;
} loom_type_identity_insertion_t;

// Prepares a payload start outside the most recently published page.
// The caller has zero-initialized the unpublished insertion record.
iree_status_t loom_type_identity_prepare_page(
    loom_module_t* module, uint64_t address,
    loom_type_identity_insertion_t* out_insertion);

// Prepares indexing of a new immutable payload whose ID is module->types.count.
// The type and index are published together after all fallible reserves. No
// other type may be interned between preparation and publication.
static inline iree_status_t loom_type_identity_prepare(
    loom_module_t* module, loom_type_t type,
    loom_type_identity_insertion_t* out_insertion) {
  *out_insertion = (loom_type_identity_insertion_t){0};
  const uint64_t address = loom_type_identity_key(type);
  if (!address) {
    return iree_ok_status();
  }
  loom_type_identity_page_t* page = module->type_identity.recent_page;
  if (!page || page->key != address >> LOOM_TYPE_IDENTITY_PAGE_SHIFT) {
    return loom_type_identity_prepare_page(module, address, out_insertion);
  }
  out_insertion->ordinal =
      &page->ordinals[(address & LOOM_TYPE_IDENTITY_PAGE_MASK) /
                      iree_max_align_t];
  // The canonical payload must not alias an earlier immutable allocation.
  IREE_ASSERT(*out_insertion->ordinal == 0);
  out_insertion->page = page;
  return iree_ok_status();
}

static inline void loom_type_identity_commit(
    loom_module_t* module, const loom_type_identity_insertion_t* insertion) {
  if (insertion->ordinal) {
    *insertion->ordinal = (uint32_t)module->types.count;
    module->type_identity.recent_page = insertion->page;
  }
  if (insertion->edge) {
    *insertion->edge = insertion->replacement;
  }
}

#ifdef __cplusplus
}
#endif

#endif  // LOOM_IR_TYPE_IDENTITY_H_
