// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Compact integer operand domain shared by CFG condition relation views.

#ifndef LOOM_ANALYSIS_CFG_CONDITION_OPERAND_DOMAIN_H_
#define LOOM_ANALYSIS_CFG_CONDITION_OPERAND_DOMAIN_H_

#include "iree/base/api.h"
#include "loom/analysis/cfg_value_identity.h"
#include "loom/analysis/condition_facts.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_CFG_CONDITION_OPERAND_INVALID UINT32_MAX

typedef uint32_t loom_cfg_condition_operand_t;

typedef struct loom_cfg_condition_operand_domain_t {
  // Borrowed local value domain used to index canonical SSA values.
  const loom_local_value_domain_t* value_domain;

  // Borrowed exact representatives used for value lookup.
  const loom_cfg_value_identity_table_t* identities;

  // Canonical SSA values ordered by local value ordinal.
  const loom_value_id_t* values;

  // Sorted distinct literal integer operands.
  const int64_t* constants;

  // Number of SSA values at the front of the operand domain.
  uint32_t value_count;

  // Number of literal integers following the SSA values.
  uint32_t constant_count;
} loom_cfg_condition_operand_domain_t;

// Returns the total number of dense operands in |domain|.
uint32_t loom_cfg_condition_operand_domain_size(
    const loom_cfg_condition_operand_domain_t* domain);

// Returns the dense operand for |value_id|, or INVALID when absent.
loom_cfg_condition_operand_t loom_cfg_condition_operand_domain_value(
    const loom_cfg_condition_operand_domain_t* domain,
    loom_value_id_t value_id);

// Returns the dense operand for |constant|, or INVALID when absent.
loom_cfg_condition_operand_t loom_cfg_condition_operand_domain_constant(
    const loom_cfg_condition_operand_domain_t* domain, int64_t constant);

// Returns the dense operand for |operand|, or INVALID when absent.
loom_cfg_condition_operand_t loom_cfg_condition_operand_domain_lookup(
    const loom_cfg_condition_operand_domain_t* domain,
    loom_condition_integer_operand_t operand);

// Resolves the authored operand and its exact ambient constant, when present.
void loom_cfg_condition_operand_domain_variants(
    const loom_cfg_condition_operand_domain_t* domain,
    const loom_value_fact_table_t* fact_table,
    loom_condition_integer_operand_t operand,
    loom_cfg_condition_operand_t out_operands[2]);

// Expands a valid dense operand to its authored condition operand.
loom_condition_integer_operand_t loom_cfg_condition_operand_domain_expand(
    const loom_cfg_condition_operand_domain_t* domain,
    loom_cfg_condition_operand_t operand);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_CFG_CONDITION_OPERAND_DOMAIN_H_
