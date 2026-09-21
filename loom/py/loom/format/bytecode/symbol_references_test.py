# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Occurrence multiplicity and source ownership over shared type graphs."""

from loom.builder import IRBuilder
from loom.dialect.test import ALL_TEST_OPS, test_array_type, test_matrix_type
from loom.format.bytecode.symbol_references import (
    SYMBOL_INTERFACE_BITS,
    SymbolReferenceProjectionBuilder,
)
from loom.ir import (
    F32,
    Block,
    DialectType,
    FunctionType,
    Region,
    SymbolName,
    SymbolNameArray,
)
from loom.verify import verify_module


def _builder() -> IRBuilder:
    builder = IRBuilder()
    builder.register_ops(ALL_TEST_OPS)
    for name in ("first", "second"):
        builder.build("test.record", attributes={"symbol": name})
    return builder


def _projection(builder: IRBuilder):
    verify_module(builder.module, ops=ALL_TEST_OPS).raise_if_errors()
    return SymbolReferenceProjectionBuilder(
        builder.module,
        {symbol.name: index for index, symbol in enumerate(builder.module.symbols)},
        {op.name: op for op in ALL_TEST_OPS},
    ).build()


def test_shared_branches_preserve_occurrence_order_and_multiplicity() -> None:
    builder = _builder()
    first = test_array_type(element_type=F32, metadata={"target": SymbolName("first")})
    second = test_array_type(
        element_type=F32, metadata={"target": SymbolName("second")}
    )
    root = FunctionType((first, second), (first,))
    expected = [0, 1, 0]
    for _ in range(5):
        root = FunctionType((root, root), ())
        expected *= 2
    value = builder.value("payload", root)
    builder.build("test.decl", func_args=[value], attributes={"callee": "shared"})
    module, rows, demands = _projection(builder)
    assert not module
    assert rows[:2] == ((), ())
    assert [entry.target_symbol_index for entry in rows[2]] == list(reversed(expected))
    assert all(entry.source_root_region_index_plus_one == 0 for entry in rows[2])
    assert all(entry.target_interfaces == 0 for entry in rows[2])
    assert not any(demands)


def test_unary_prefixes_share_one_nonempty_summary() -> None:
    builder = _builder()
    root = test_array_type(element_type=F32, metadata={"target": SymbolName("first")})
    values = []
    for index in range(2048):
        root = DialectType("test.ref", (root,))
        values.append(builder.value(f"payload{index}", root))
    builder.build("test.decl", func_args=values, attributes={"callee": "prefixes"})
    _, rows, _ = _projection(builder)
    assert len(rows[2]) == 2048
    assert all(entry.target_symbol_index == 0 for entry in rows[2])


def test_descriptor_roles_and_projected_root_ownership() -> None:
    builder = _builder()
    available = test_matrix_type(
        element_type=F32, scope="subgroup", rows=4, target=SymbolName("first")
    )
    value = builder.value("payload", available)
    regions = [Region(blocks=[Block()]), Region(blocks=[Block()])]
    builder.build(
        "test.split_func",
        func_args=[value],
        attributes={"callee": "projected"},
        regions=regions,
    )
    references = SymbolNameArray(
        SymbolName(name) for name in ("first", "second", "first")
    )
    for region in regions:
        builder.set_insertion_block(region.blocks[0])
        builder.build(
            "test.symbol_array_attrs",
            attributes={"dependencies": references, "available": references},
        )
        builder.build("test.yield")
    _, rows, _ = _projection(builder)
    assert [
        (entry.source_root_region_index_plus_one, entry.target_symbol_index)
        for entry in rows[2]
    ] == [(2, 0), (2, 1), (2, 0), (1, 0), (1, 1), (1, 0)]
    assert all(
        entry.target_interfaces == SYMBOL_INTERFACE_BITS["record"] for entry in rows[2]
    )
