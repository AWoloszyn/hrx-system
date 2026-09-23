# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact packed selection through typed memory, including floating payload bits."""

import argparse
from pathlib import Path

from loom.gen.test.kernel_fixture import Arrays, Case, signed_bits


def selection(arrays, element, width, count):
    name = f"select_{element.lower()}_{count}"
    storage = f"i{width}"
    size = 256 * count
    # Every byte position sees every bit pattern. Halfwords include signed zero,
    # subnormals, infinities and both quiet/signaling NaN payloads in f16/bf16.
    special = [0, 0x8000, 1, 0x8001, 0x7C00, 0x7C01, 0x7E01, 0x7F80, 0x7F81, 0x7FC1, 0xFFFF]
    masks = [(packet >> lane) & 1 for packet in range(256) for lane in range(count)]
    true_values = []
    false_values = []
    for packet in range(256):
        for lane in range(count):
            bits = special[(packet + lane) % len(special)] if width == 16 and packet < 32 else packet * 73 + lane * 257
            true_values.append(signed_bits(bits, width))
            false_values.append(signed_bits(~bits, width))
    expected = [true_value if mask else false_value for mask, true_value, false_value in zip(masks, true_values, false_values, strict=True)]
    payload_type = f"vector<{count}x{element}>"
    condition_type = f"vector<{count}xi32>"
    view_type = f"view<{size}x{element}>"
    loads = "\n".join(f"  %{arm}_value = vector.load %{arm}_view[%position] : {view_type} -> {payload_type}" for arm in ["true", "false"])
    stores = f"  vector.store %result, %output_view[%position] : {payload_type}, {view_type}"
    if count in [3, 7]:
        # Partial aggregates cross scalar memory accesses; vector memory has a
        # separate contract for complete packed words.
        positions = []
        loads = []
        stores = []
        for lane in range(count):
            positions.extend([f"  %element_offset_{lane} = index.constant {lane} : index", f"  %element_position_{lane} = index.add %position, %element_offset_{lane} : index"])
            for arm in ["true", "false"]:
                loads.append(f"  %{arm}_{lane} = view.load %{arm}_view[%element_position_{lane}] : {view_type} -> {element}")
            stores.extend(
                [f"  %result_{lane} = vector.extract %result[{lane}] : {payload_type} -> {element}", f"  view.store %result_{lane}, %output_view[%element_position_{lane}] : {element}, {view_type}"]
            )
        for arm in ["true", "false"]:
            loads.append(f"  %{arm}_value = vector.from_elements " + ", ".join(f"%{arm}_{lane}" for lane in range(count)) + f" : {payload_type}")
        loads = "\n".join(positions + loads)
        stores = "\n".join(stores)
    kernel = f"""kernel.def @{name}() {{
  %unit = index.constant 1 : index
  %width = index.constant 32 : index
  %groups = index.constant 8 : index
  kernel.launch.config workgroups(%groups, %unit, %unit) workgroup_size(%width, %unit, %unit) : index
}} launch(%conditions: buffer, %true_input: buffer, %false_input: buffer, %output: buffer) {{
  %group = kernel.workgroup.id<x> : index
  %lane = kernel.workitem.id<x> : index
  %width = index.constant 32 : index
  %group_offset = index.mul %group, %width : index
  %packet = index.add %group_offset, %lane : index
  %count = index.constant {count} : index
  %position = index.mul %packet, %count : index
  %base = index.constant 0 : offset
  %conditions_view = buffer.view %conditions[%base] : buffer -> view<{size}xi32>
  %true_view = buffer.view %true_input[%base] : buffer -> {view_type}
  %false_view = buffer.view %false_input[%base] : buffer -> {view_type}
  %output_view = buffer.view %output[%base] : buffer -> {view_type}
  %conditions_value = vector.load %conditions_view[%position] : view<{size}xi32> -> {condition_type}
  %inactive = scalar.constant 0 : i32
  %inactive_lanes = vector.splat %inactive : {condition_type}
  %mask = vector.cmpi ne, %conditions_value, %inactive_lanes : {condition_type} -> vector<{count}xi1>
{loads}
  %result = vector.select %mask, %true_value, %false_value : {payload_type}
{stores}
  kernel.return
}}

"""
    case = Case(arrays, name + "_bits", storage, size)
    case.array("conditions", masks, "i32")
    case.array("true_input", true_values)
    case.array("false_input", false_values)
    case.launch(name, "%conditions, %true_input, %false_input, %output", f"tensor<{size}xi32>, tensor<{size}x{storage}>, tensor<{size}x{storage}>, tensor<{size}x{storage}>")
    return kernel + case.finish(expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arrays", type=Path, required=True)
    options = parser.parse_args()
    arrays = Arrays(options.arrays, options.output.parent)
    cases = [selection(arrays, element, width, count) for element, width in [("i8", 8), ("i16", 16), ("f16", 16), ("bf16", 16), ("f8E4M3", 8), ("f8E5M2", 8)] for count in [1, 3, 4, 7, 8]]
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text("\n".join(cases))


if __name__ == "__main__":
    main()
