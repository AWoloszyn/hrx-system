# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Mixed static type/encoding ordering and invocation-owned catalog identity."""

from dataclasses import replace

import pytest

from loom.builder import IRBuilder
from loom.dialect.test import ALL_TEST_OPS, test_array_type
from loom.format.bytecode.reader import BytecodeError, BytecodeReader, read_module
from loom.format.bytecode.writer import (
    ATTR_KIND_TYPE,
    BYTECODE_TYPE_KIND_BY_IR_KIND,
    BytecodeWriter,
    write_module,
)
from loom.ir import (
    F32,
    DynamicDim,
    EncodingInstance,
    FunctionType,
    Module,
    ShapedType,
    StaticDim,
    SymbolName,
    Type,
    TypeKind,
)


def _chain(count: int) -> Type:
    root: Type = F32
    for _ in range(count):
        encoding = EncodingInstance("typed", params=(("element", root),))
        root = ShapedType(TypeKind.TILE, F32, (StaticDim(4),), encoding)
    return root


def _record_module(**attributes: Type) -> Module:
    builder = IRBuilder()
    builder.register_ops(ALL_TEST_OPS)
    builder.build("test.record", attributes={"symbol": "owner", "dict": attributes})
    return builder.module


@pytest.mark.parametrize("count", [1, 4, 2048])
def test_mixed_chain_roundtrip_without_mutating_source(count: int) -> None:
    module = _record_module(root=_chain(count))
    assert not module.encodings
    data = write_module(module, op_decls=ALL_TEST_OPS)
    assert not module.encodings
    assert write_module(module, op_decls=ALL_TEST_OPS) == data
    loaded = read_module(data, op_decls=ALL_TEST_OPS)
    assert len(loaded.encodings) == count
    root = loaded.symbols[0].op.attributes["dict"]["root"]
    for index in reversed(range(count)):
        assert isinstance(root, ShapedType)
        assert root.dims == (StaticDim(4),)
        assert root.encoding is loaded.encodings[index]
        root = root.encoding.params[0][1]
    assert root == F32


def test_independent_shared_mixed_graphs_have_one_catalog() -> None:
    roots = []
    count = 1024
    for _ in range(2):
        root: Type = F32
        for _ in range(count):
            pair = FunctionType((root, root), ())
            encoding = EncodingInstance("typed", params=(("element", pair),))
            root = ShapedType(TypeKind.TILE, F32, (StaticDim(4),), encoding)
        roots.append(root)
    module = _record_module(left=roots[0], right=roots[1])
    loaded = read_module(
        write_module(module, op_decls=ALL_TEST_OPS), op_decls=ALL_TEST_OPS
    )
    attributes = loaded.symbols[0].op.attributes["dict"]
    assert attributes["left"] is attributes["right"]
    assert len(loaded.encodings) == count
    root = attributes["left"]
    for _ in range(count):
        pair = root.encoding.params[0][1]
        assert pair.arg_types[0] is pair.arg_types[1]
        root = pair.arg_types[0]
    assert root == F32


def test_authored_catalog_deduplicates_and_preserves_alias_without_mutation() -> None:
    encoding = EncodingInstance("typed", params=(("element", F32),))
    aliased = replace(encoding, alias="payload_layout")
    module = Module(encodings=[encoding, aliased])
    loaded = read_module(write_module(module))
    assert len(loaded.encodings) == 1
    assert loaded.encodings[0].alias == "payload_layout"
    assert module.encodings[0] is encoding
    assert module.encodings[1] is aliased


def test_distinct_encodings_can_share_the_same_completed_type_prefix() -> None:
    first = EncodingInstance("typed", params=(("element", F32),))
    second = EncodingInstance("wrapped", params=(("base", first),))
    module = Module(encodings=[second])
    loaded = read_module(write_module(module))
    assert len(loaded.encodings) == 2
    assert loaded.encodings[1].params[0][1] is loaded.encodings[0]
    assert module.encodings == [second]


def test_encoding_alias_conflict_is_rejected() -> None:
    module = Module(
        encodings=[
            EncodingInstance("typed", alias="layout"),
            EncodingInstance("different", alias="layout"),
        ]
    )
    with pytest.raises(ValueError, match="already names a different encoding"):
        write_module(module)


def test_static_encoding_cannot_capture_a_bound_type() -> None:
    bound = ShapedType(TypeKind.VECTOR, F32, (DynamicDim(0),))
    module = Module(encodings=[EncodingInstance("typed", params=(("element", bound),))])
    with pytest.raises(ValueError, match="static encoding parameters cannot capture"):
        write_module(module)


def test_discovered_encodings_keep_module_symbol_dependency_ownership() -> None:
    builder = IRBuilder()
    builder.register_ops(ALL_TEST_OPS)
    builder.build("test.record", attributes={"symbol": "dependency"})
    element = test_array_type(
        element_type=F32, metadata={"target": SymbolName("dependency")}
    )
    encoding = EncodingInstance("typed", params=(("element", element),))
    builder.build(
        "test.record", attributes={"symbol": "owner", "dict": {"layout": encoding}}
    )
    writer = BytecodeWriter(builder.module, op_decls=ALL_TEST_OPS)
    assert not builder.module.encodings
    assert len(writer._ctx.encodings) == 1
    assert [row.target_symbol_index for row in writer._module_dependencies] == [0]
    assert [row.target_symbol_index for row in writer._symbol_dependencies[1]] == [0]


@pytest.mark.parametrize("prefixes", [(0, 2), (2, 2), (1, 0), (1, 3)])
def test_mixed_table_order_is_validated(prefixes: tuple[int, int]) -> None:
    type_data = bytes(
        [
            2,
            BYTECODE_TYPE_KIND_BY_IR_KIND[TypeKind.SCALAR],
            F32.kind.value,
            BYTECODE_TYPE_KIND_BY_IR_KIND[TypeKind.TILE],
            F32.kind.value,
            1,
            1,
            1,
            0,
            4,  # Rank, static encoding attachment, ID, dimension.
        ]
    )
    encoding_data = bytes(
        [
            1,
            1,
            2,  # Family count, name, instance count.
            prefixes[0],
            0,
            0,
            1,
            2,
            ATTR_KIND_TYPE,
            0,
            prefixes[1],
            0,
            0,
            1,
            2,
            ATTR_KIND_TYPE,
            1,
        ]
    )
    reader = BytecodeReader(b"")
    reader._strings = ["", "typed", "element"]
    with pytest.raises(
        BytecodeError, match=r"prior type|static encoding id|type prefix"
    ):
        reader._read_types_and_encodings((0, type_data), (0, encoding_data))
