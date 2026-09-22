// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Attribute readers used by generated Low packet APIs.

#ifndef LOOM_CODEGEN_LOW_IMMEDIATE_FIELDS_H_
#define LOOM_CODEGEN_LOW_IMMEDIATE_FIELDS_H_

#include "loom/ir/attribute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Indices use canonical dictionary order, distinct from declaration order in
// the descriptor's immediate table. Generated readers exist only for
// required prefixes and a trailing optional field, with agreement across every
// descriptor variant. Fields after an omitted key have no fixed position.
//
// Reads a required immediate from a verified packet dictionary. The returned
// attribute borrows any backing storage from the packet's owning module.
static inline loom_attribute_t loom_low_immediate_attr(
    loom_named_attr_slice_t attributes, uint16_t index) {
  return attributes.entries[index].value;
}

// Reads a trailing optional immediate, preserving omission as ABSENT. The
// descriptor's default value is a separate semantic from authored presence.
static inline loom_attribute_t loom_low_optional_immediate_attr(
    loom_named_attr_slice_t attributes, uint16_t index) {
  return attributes.count > index ? attributes.entries[index].value
                                  : loom_attr_absent();
}

#ifdef __cplusplus
}
#endif

#endif  // LOOM_CODEGEN_LOW_IMMEDIATE_FIELDS_H_
