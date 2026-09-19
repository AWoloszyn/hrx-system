// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/type_identity.h"

#include "iree/base/internal/math.h"

// Compressed sixteen-way page directory. Odd edges tag page pointers; even
// edges are branch pointers. Empty edges need no branch allocation.
typedef struct loom_type_identity_branch_t {
  // Representative page number supplying the compressed low-bit prefix.
  uint64_t key;
  // Children selected by the differing page-number nibble.
  uint64_t children[16];
  // Nibble offset, strictly increasing along any branch path.
  uint32_t shift;
} loom_type_identity_branch_t;

static loom_type_identity_branch_t* loom_type_identity_branch(uint64_t edge) {
  return (loom_type_identity_branch_t*)(uintptr_t)edge;
}

static loom_type_identity_page_t* loom_type_identity_page(uint64_t edge) {
  return (loom_type_identity_page_t*)(uintptr_t)(edge & ~UINT64_C(1));
}

loom_type_id_t loom_type_identity_find(const loom_module_t* module,
                                       loom_type_t type) {
  const uint64_t address = loom_type_identity_key(type);
  if (!address) {
    return LOOM_TYPE_ID_INVALID;
  }
  const uint64_t key = address >> LOOM_TYPE_IDENTITY_PAGE_SHIFT;
  const loom_type_identity_page_t* page = module->type_identity.recent_page;
  if (!page || page->key != key) {
    uint64_t edge = module->type_identity.root;
    while (edge && !(edge & 1)) {
      const loom_type_identity_branch_t* branch =
          loom_type_identity_branch(edge);
      edge = branch->children[(key >> branch->shift) & 15];
    }
    if (!edge) {
      return LOOM_TYPE_ID_INVALID;
    }
    page = loom_type_identity_page(edge);
    if (page->key != key) {
      return LOOM_TYPE_ID_INVALID;
    }
  }
  const uint32_t ordinal =
      page->ordinals[(address & LOOM_TYPE_IDENTITY_PAGE_MASK) /
                     iree_max_align_t];
  if (!ordinal) {
    return LOOM_TYPE_ID_INVALID;
  }
  const loom_type_id_t id = ordinal - 1;
  const loom_type_t canonical = module->types.entries[id];
  return canonical.header == type.header &&
                 canonical.encoding_id == type.encoding_id &&
                 canonical.encoding_flags == type.encoding_flags &&
                 canonical.dims[0] == type.dims[0] &&
                 canonical.dims[1] == type.dims[1]
             ? id
             : LOOM_TYPE_ID_INVALID;
}

iree_status_t loom_type_identity_prepare_page(
    loom_module_t* module, uint64_t address,
    loom_type_identity_insertion_t* out_insertion) {
  const uint64_t key = address >> LOOM_TYPE_IDENTITY_PAGE_SHIFT;
  loom_type_identity_page_t* page = NULL;
  uint64_t* edge = &module->type_identity.root;
  uint64_t existing_key = 0;
  while (*edge && !(*edge & 1)) {
    loom_type_identity_branch_t* branch = loom_type_identity_branch(*edge);
    existing_key = branch->key;
    if ((key ^ existing_key) & ((UINT64_C(1) << branch->shift) - 1)) {
      break;
    }
    edge = &branch->children[(key >> branch->shift) & 15];
  }
  if (*edge & 1) {
    page = loom_type_identity_page(*edge);
    existing_key = page->key;
  }
  if (!page || page->key != key) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate(&module->arena, sizeof(*page), (void**)&page));
    *page = (loom_type_identity_page_t){.key = key};
    out_insertion->edge = edge;
    out_insertion->replacement = (uint64_t)(uintptr_t)page | 1;
    if (*edge) {
      const uint32_t shift =
          (uint32_t)iree_math_count_trailing_zeros_u64(key ^ existing_key) &
          ~3u;
      loom_type_identity_branch_t* branch = NULL;
      IREE_RETURN_IF_ERROR(iree_arena_allocate(&module->arena, sizeof(*branch),
                                               (void**)&branch));
      *branch =
          (loom_type_identity_branch_t){.key = existing_key, .shift = shift};
      branch->children[(key >> shift) & 15] = out_insertion->replacement;
      branch->children[(existing_key >> shift) & 15] = *edge;
      out_insertion->replacement = (uint64_t)(uintptr_t)branch;
    }
  }
  out_insertion->ordinal =
      &page->ordinals[(address & LOOM_TYPE_IDENTITY_PAGE_MASK) /
                      iree_max_align_t];
  // Only a freshly allocated canonical payload can be published. A nonzero
  // slot would alias a still-live immutable type, not a recoverable miss.
  IREE_ASSERT(*out_insertion->ordinal == 0);
  out_insertion->page = page;
  return iree_ok_status();
}
