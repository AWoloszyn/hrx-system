// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/callable_effects.h"
#include "loom/ops/template/ops.h"
#include "loom/rewrite/rewriter.h"

iree_status_t loom_template_apply_canonicalize(loom_op_t* op,
                                               loom_rewriter_t* rewriter) {
  if (loom_template_apply_purity(op) != 0 ||
      !loom_callable_effects_callee_is_pure(rewriter->module,
                                            loom_template_apply_family(op))) {
    return iree_ok_status();
  }
  return loom_template_apply_rewrite_purity(
      rewriter, op, loom_attr_enum(LOOM_TEMPLATE_PURITY_PURE));
}

iree_status_t loom_template_call_canonicalize(loom_op_t* op,
                                              loom_rewriter_t* rewriter) {
  if (loom_template_call_purity(op) != 0 ||
      !loom_callable_effects_callee_is_pure(rewriter->module,
                                            loom_template_call_callee(op))) {
    return iree_ok_status();
  }
  return loom_template_call_rewrite_purity(
      rewriter, op, loom_attr_enum(LOOM_TEMPLATE_PURITY_PURE));
}

loom_trait_flags_t loom_template_apply_effective_traits(const loom_op_t* op) {
  return LOOM_TRAIT_CONTEXTUAL |
         loom_callable_effects_traits(loom_template_apply_purity(op));
}

loom_trait_flags_t loom_template_call_effective_traits(const loom_op_t* op) {
  return LOOM_TRAIT_CONTEXTUAL |
         loom_callable_effects_traits(loom_template_call_purity(op));
}
