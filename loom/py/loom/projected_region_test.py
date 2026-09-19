# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

from loom.builder import IRBuilder
from loom.builtin_types import ALL_BUILTIN_TYPES
from loom.dialect.test import ALL_TEST_OPS, test_split_func
from loom.format.bytecode.reader import read_module
from loom.format.bytecode.writer import write_module
from loom.format.text.parser import Parser
from loom.format.text.printer import Printer
from loom.ir import (
    ENCODING_TYPE,
    F32,
    I32,
    INDEX,
    Block,
    DynamicDim,
    DynamicEncoding,
    Module,
    Operation,
    ShapedType,
    TypeKind,
)
from loom.ir import Region as IRRegion
from loom.verify import verify_module


def test_region_arg_source_accepts_func_args_field() -> None:
    assert test_split_func.regions[0].arg_source == "args"


def test_builder_seeds_projected_func_args_region() -> None:
    builder = IRBuilder(insertion_block=Block())
    builder.register_ops(ALL_TEST_OPS)
    builder.register_types(ALL_BUILTIN_TYPES)

    x = builder.value("x", I32)
    config = IRRegion(blocks=[Block()])
    body = IRRegion(blocks=[Block()])
    builder.build(
        "test.split_func",
        func_args=[x],
        attributes={"callee": "projected"},
        regions=[config, body],
    )

    assert body.blocks[0].arg_ids == [x.id]
    assert len(config.blocks[0].arg_ids) == 1
    config_arg_id = config.blocks[0].arg_ids[0]
    assert config_arg_id != x.id
    assert builder.module.values[config_arg_id].name == "x"
    assert builder.module.values[config_arg_id].type == I32
    assert builder.module.values[config_arg_id].is_block_arg


def test_bytecode_preserves_projected_func_regions() -> None:
    parser = Parser()
    parser.register_ops(ALL_TEST_OPS)
    parser.register_types(ALL_BUILTIN_TYPES)
    module = parser.parse(
        "test.split_func @projected(%arg: i32, %other: index) {\n"
        "  test.use %arg : i32\n"
        "  test.use %other : index\n"
        "  test.yield\n"
        "} launch {\n"
        "  test.use %arg : i32\n"
        "  test.use %other : index\n"
        "  test.yield\n"
        "}\n"
    )
    loaded = read_module(write_module(module))

    op = loaded.symbols[0].op
    assert op is not None
    assert len(op.regions) == 2
    config_arg_ids = op.regions[0].blocks[0].arg_ids
    body_arg_ids = op.regions[1].blocks[0].arg_ids
    assert len(body_arg_ids) == len(config_arg_ids) == 2
    assert body_arg_ids[0] != config_arg_ids[0]
    assert loaded.values[body_arg_ids[0]].name == "arg"
    assert loaded.values[config_arg_ids[0]].name == "arg"


@pytest.mark.parametrize("construction", ["parse", "build"])
def test_projected_signature_bindings_follow_region_identities(
    construction: str,
) -> None:
    parser = Parser()
    printer = Printer()
    for format in (parser, printer):
        format.register_ops(ALL_TEST_OPS)
        format.register_types(ALL_BUILTIN_TYPES)
    if construction == "parse":
        module = parser.parse(
            "test.split_func @f(%extent: index, %layout: encoding, "
            "%input: tile<[%extent]xf32, %layout>) {\n"
            "  test.yield\n"
            "} launch {\n"
            "  test.yield\n"
            "}\n",
            verify=True,
        )
    else:
        builder = IRBuilder()
        builder.register_ops(ALL_TEST_OPS)
        extent = builder.value("extent", INDEX)
        layout = builder.value("layout", ENCODING_TYPE)
        input = builder.value(
            "input",
            ShapedType(TypeKind.TILE, F32, (DynamicDim(),), DynamicEncoding()),
            dim_bindings={0: extent.id},
            encoding_binding=layout.id,
        )
        builder.build(
            "test.split_func",
            func_args=[extent, layout, input],
            attributes={"callee": "f"},
            regions=[
                IRRegion(blocks=[Block(ops=[Operation(name="test.yield")])]),
                IRRegion(blocks=[Block(ops=[Operation(name="test.yield")])]),
            ],
        )
        module = builder.module
        verify_module(module, ops=ALL_TEST_OPS).raise_if_errors()

    def check(candidate: Module) -> None:
        config, body = candidate.body.ops[0].regions
        assert set(config.blocks[0].arg_ids).isdisjoint(body.blocks[0].arg_ids)
        for region in (config, body):
            extent, layout, value = region.blocks[0].arg_ids
            assert candidate.values[value].dim_bindings == {0: extent}
            assert candidate.values[value].encoding_binding == layout

    text = printer.print_module(module)
    for candidate in (
        module,
        parser.parse(text, verify=True),
        read_module(write_module(module)),
    ):
        check(candidate)
