// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/kernel.h>
#include <loomcxx/view.h>

namespace loomt = loom::type;

using rows8 = loomt::view<const float, loomt::dynamic, 8>;

static rows8 preserve_view(rows8 source) { return source; }

static float read_view(rows8 source) { return loom::view::load(source, 2, 3); }

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void typed_view_static_layouts(const float* input, float* output,
                               unsigned input_origin) {
  loom::assume(input_origin < 16);
  auto dense = loom::encoding::layout::dense<2>();
  auto strided = loom::encoding::layout::strided(11u, 1u);
  auto dense_source = preserve_view(
      loom::buffer::view<loomt::dynamic, 8>(input + input_origin, {3}, dense));
  auto strided_source = preserve_view(loom::buffer::view<loomt::dynamic, 8>(
      input + input_origin, {3}, strided));
  output[0] = read_view(dense_source);
  output[1] = read_view(strided_source);
}

[[loom::kernel, loom::workgroup_size(8, 8, 1), loom::workgroup_count(1, 1, 1)]]
void typed_view_copy(const float* input, float* output, unsigned rows,
                     unsigned input_stride, unsigned input_origin,
                     unsigned output_origin) {
  loom::assume(rows < 9);
  loom::assume(input_stride < 32);
  loom::assume(input_origin < 16);
  loom::assume(output_origin < 16);

  auto input_layout = loom::encoding::layout::strided(input_stride, 1u);
  auto output_layout = loom::encoding::layout::dense<2>();
  auto source = preserve_view(loom::buffer::view<loomt::dynamic, 8>(
      input + input_origin, {rows}, input_layout));
  auto destination = loom::buffer::view<loomt::dynamic, 8>(
      output + output_origin, {rows}, output_layout);

  loomt::size_type row = loom::workitem_id.y;
  loomt::size_type column = loom::workitem_id.x;
  if (row < rows) {
    float value = loom::view::load(source, row, column);
    loom::view::store(value, destination, row, column);
  }
}
