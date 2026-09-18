// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_FORMAT_TEXT_PRINTER_NAME_PLAN_H_
#define LOOM_FORMAT_TEXT_PRINTER_NAME_PLAN_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"
#include "loom/util/stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_print_name_resolution_t loom_print_name_resolution_t;

// Canonical SSA name resolutions shared by one print invocation.
//
// Construction indexes explicit names by parser scope and resolves all
// same-scope duplicates, shadowed captures, and generated-name collisions once.
// A scope walk checks ordinary and embedded type/attribute references against
// indexed bindings; captured values receive unambiguous spellings. Generated
// candidates have disjoint value-ID suffixes and cannot equal explicit names in
// any scope. Emission is then an O(1) value-id lookup. The plan owns only the
// resolutions in a scoped arena backed by the module block pool; its
// construction index is discarded from that arena before initialization
// returns.
typedef struct loom_print_name_plan_t {
  // Canonical resolution for each module value ID.
  // NULL when the module has no explicit value names.
  loom_print_name_resolution_t* resolutions;
  // Print-scoped arena owning resolutions. A NULL block pool marks a zeroed,
  // unprepared plan; standalone atom printers prepare it on the first SSA ref.
  iree_arena_allocator_t arena;
} loom_print_name_plan_t;

// Builds canonical SSA name resolutions for |module|.
iree_status_t loom_print_name_plan_initialize(const loom_module_t* module,
                                              loom_print_name_plan_t* out_plan);

// Releases storage owned by |plan|, including an unprepared zeroed plan.
void loom_print_name_plan_deinitialize(loom_print_name_plan_t* plan);

// Prints |value_id|'s canonical SSA reference.
//
// A zeroed |plan| is prepared lazily on the first valid reference and must be
// deinitialized by the caller. All references in a composite type or attribute
// share that plan; scalar atoms require no module traversal or allocation.
iree_status_t loom_print_name_plan_write_value_ref(loom_print_name_plan_t* plan,
                                                   loom_output_stream_t* stream,
                                                   const loom_module_t* module,
                                                   loom_value_id_t value_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_TEXT_PRINTER_NAME_PLAN_H_
