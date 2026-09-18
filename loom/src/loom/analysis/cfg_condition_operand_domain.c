// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/cfg_condition_operand_domain.h"

uint32_t loom_cfg_condition_operand_domain_size(
    const loom_cfg_condition_operand_domain_t* domain) {
  return domain->value_count + domain->constant_count;
}

loom_cfg_condition_operand_t loom_cfg_condition_operand_domain_value(
    const loom_cfg_condition_operand_domain_t* domain,
    loom_value_id_t value_id) {
  const loom_value_id_t canonical =
      loom_cfg_value_identity_table_lookup(domain->identities, value_id);
  const loom_value_ordinal_t ordinal =
      loom_local_value_domain_try_ordinal(domain->value_domain, canonical);
  if (ordinal == LOOM_VALUE_ORDINAL_INVALID) {
    return LOOM_CFG_CONDITION_OPERAND_INVALID;
  }
  uint32_t begin = 0;
  uint32_t end = domain->value_count;
  while (begin < end) {
    const uint32_t middle = begin + (end - begin) / 2;
    const loom_value_ordinal_t middle_ordinal = loom_local_value_domain_ordinal(
        domain->value_domain, domain->values[middle]);
    if (middle_ordinal < ordinal) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return begin < domain->value_count && domain->values[begin] == canonical
             ? begin
             : LOOM_CFG_CONDITION_OPERAND_INVALID;
}

loom_cfg_condition_operand_t loom_cfg_condition_operand_domain_constant(
    const loom_cfg_condition_operand_domain_t* domain, int64_t constant) {
  uint32_t begin = 0;
  uint32_t end = domain->constant_count;
  while (begin < end) {
    const uint32_t middle = begin + (end - begin) / 2;
    if (domain->constants[middle] < constant) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return begin < domain->constant_count && domain->constants[begin] == constant
             ? domain->value_count + begin
             : LOOM_CFG_CONDITION_OPERAND_INVALID;
}

loom_cfg_condition_operand_t loom_cfg_condition_operand_domain_lookup(
    const loom_cfg_condition_operand_domain_t* domain,
    loom_condition_integer_operand_t operand) {
  return operand.kind == LOOM_CONDITION_INTEGER_OPERAND_VALUE
             ? loom_cfg_condition_operand_domain_value(domain, operand.value_id)
             : loom_cfg_condition_operand_domain_constant(domain,
                                                          operand.constant);
}

void loom_cfg_condition_operand_domain_variants(
    const loom_cfg_condition_operand_domain_t* domain,
    const loom_value_fact_table_t* fact_table,
    loom_condition_integer_operand_t operand,
    loom_cfg_condition_operand_t out_operands[2]) {
  out_operands[0] = loom_cfg_condition_operand_domain_lookup(domain, operand);
  out_operands[1] = LOOM_CFG_CONDITION_OPERAND_INVALID;
  if (operand.kind != LOOM_CONDITION_INTEGER_OPERAND_VALUE ||
      fact_table == NULL) {
    return;
  }
  int64_t exact_value = 0;
  if (!loom_value_facts_as_exact_i64(
          loom_value_fact_table_lookup(fact_table, operand.value_id),
          &exact_value)) {
    return;
  }
  const loom_cfg_condition_operand_t constant =
      loom_cfg_condition_operand_domain_constant(domain, exact_value);
  if (constant != out_operands[0]) {
    out_operands[1] = constant;
  }
}

loom_condition_integer_operand_t loom_cfg_condition_operand_domain_expand(
    const loom_cfg_condition_operand_domain_t* domain,
    loom_cfg_condition_operand_t operand) {
  if (operand < domain->value_count) {
    return (loom_condition_integer_operand_t){
        .kind = LOOM_CONDITION_INTEGER_OPERAND_VALUE,
        .value_id = domain->values[operand],
    };
  }
  return (loom_condition_integer_operand_t){
      .kind = LOOM_CONDITION_INTEGER_OPERAND_CONSTANT,
      .value_id = LOOM_VALUE_ID_INVALID,
      .constant = domain->constants[operand - domain->value_count],
  };
}
