// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_LOOP_SCHEDULE_H_
#define LOOM_IMPORT_CXX_BINDING_LOOP_SCHEDULE_H_

#include <span>

#include "loom/import/cxx/source/source.h"
#include "loom/ops/scf/ops.h"

namespace loom::cxx_import {

// Admits source scheduling attributes once at their loop and projects them
// directly to scf.for policies. The source owns the choice; no heuristic picks
// a factor, depth, or ordering on the importer's behalf.
class LoopSchedule {
 public:
  LoopSchedule(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
               cxx::List<cxx::AttributeSpecifierAST*>* attributes);

  bool empty() const { return flags_ == 0; }

  // Builds the counted loop and its explicit index-valued scheduling operands.
  // Throws StatusError for fallible builder operations.
  loom_op_t* build(loom_builder_t* builder, loom_value_id_t lower,
                   loom_value_id_t upper, loom_value_id_t step,
                   std::span<const loom_value_id_t> initial,
                   loom_location_id_t location) const;

 private:
  // Presence flags distinguish absent policies from explicit serial values.
  loom_scf_for_build_flags_t flags_ = 0;
  // Positive source constant when the pipeline-depth operand is present.
  int32_t pipeline_depth_ = 0;
  // Positive source constant when the bounded unroll operand is present.
  int32_t unroll_factor_ = 0;
  // Full-unroll policy when the bare source attribute is present.
  uint8_t unroll_policy_ = 0;
  // Explicit source ordering for cloned loop bodies, when present.
  uint8_t unroll_schedule_ = 0;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_LOOP_SCHEDULE_H_
