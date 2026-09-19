// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_VALUE_REPRESENTATION_H_
#define LOOM_IMPORT_CXX_VALUE_REPRESENTATION_H_

#include <span>
#include <vector>

#include "loom/ir/types.h"

namespace loom::cxx_import {

// An interior source pointer retains its allocation identity separately from
// its nonnegative object-relative byte origin. Neither component is an address.
struct Pointer {
  // Native buffer identifying the original storage allocation.
  loom_value_id_t root;
  // Native offset from that root, including a possible one-past origin.
  loom_value_id_t byte_offset;
};

// Source values flatten to ordinary High operands at calls and region edges.
// Scalars have one component; pointers and array designators have two. The
// source type admits operations before a consumer extracts either form.
class Value {
 public:
  Value() = default;
  Value(loom_value_id_t scalar) : components_{scalar, LOOM_VALUE_ID_INVALID} {}
  Value(Pointer pointer) : components_{pointer.root, pointer.byte_offset} {}
  explicit Value(std::span<const loom_value_id_t> components)
      : components_{components[0], components.size() == 2
                                       ? components[1]
                                       : LOOM_VALUE_ID_INVALID} {}

  bool is_pointer() const { return components_[1] != LOOM_VALUE_ID_INVALID; }
  loom_value_id_t scalar() const {
    IREE_ASSERT(!is_pointer());
    return components_[0];
  }
  Pointer pointer() const {
    IREE_ASSERT(is_pointer());
    return {components_[0], components_[1]};
  }
  std::span<const loom_value_id_t> components() const {
    return {components_, is_pointer() ? 2u : 1u};
  }
  void append_to(std::vector<loom_value_id_t>& values) const {
    auto source = components();
    values.insert(values.end(), source.begin(), source.end());
  }

 private:
  // Inline scalar or buffer/offset pair; an invalid offset denotes a scalar.
  loom_value_id_t components_[2] = {LOOM_VALUE_ID_INVALID,
                                    LOOM_VALUE_ID_INVALID};
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_VALUE_REPRESENTATION_H_
