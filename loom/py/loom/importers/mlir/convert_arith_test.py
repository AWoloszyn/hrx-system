# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

import loom
from loom.importers.mlir.api import import_iree_ir
from loom.importers.mlir.convert_arith import convert_index_cast, convert_unary
from loom.importers.mlir.model import MlirConversionContext, SourceOp
from loom.ir import I32, ShapedType, StaticDim, Type, TypeKind
from loom.verify import verify_module


class _MlirOperation:
    name = "arith.index_castui"

    def get_asm(self, *, enable_debug_info: bool, use_local_scope: bool) -> str:
        return "%result = arith.index_castui %input : i32 to index"


def test_unsigned_index_cast_is_not_silently_reinterpreted_as_signed() -> None:
    _, builder = loom.module_builder()
    context = MlirConversionContext.with_prelude(builder, {})
    source_op = SourceOp(
        source=_MlirOperation(),
        depth=1,
    )

    assert convert_index_cast(source_op, context)

    blocked = context.finish().blocked
    assert len(blocked) == 1
    assert blocked[0].source == ("%result = arith.index_castui %input : i32 to index")
    assert blocked[0].target == (
        "arith.index_castui cannot map to signed Loom index.cast semantics",
    )


@pytest.mark.parametrize("source_name", ["ctlz", "ctpop", "cttz"])
@pytest.mark.parametrize(
    ("source_type", "result_type"),
    [
        ("i32", I32),
        ("vector<4xi32>", ShapedType(TypeKind.VECTOR, I32, (StaticDim(4),))),
    ],
)
def test_unary_import_preserves_resolved_types(
    source_name: str, source_type: str, result_type: Type
) -> None:
    ir = import_iree_ir()

    with ir.Context():
        source = ir.Module.parse(
            f"func.func @count(%arg: {source_type}) -> {source_type} {{\n"
            f"  %result = math.{source_name} %arg : {source_type}\n"
            f"  return %result : {source_type}\n"
            "}"
        )
        block = source.body.operations[0].regions[0].blocks[0]
        module, builder = loom.module_builder()
        argument = builder.value("arg", result_type)
        body = builder.region()
        builder.func.def_(callee="count", args=[argument], results=[], body=body)
        context = MlirConversionContext.with_prelude(
            builder, {block.arguments[0]: (argument, source_type)}
        )
        operation = SourceOp(block.operations[0], depth=1)

        with builder.insertion_block(body.blocks[0]):
            assert convert_unary(operation, context)
            builder.func.return_()

        result = context.mapped(operation.result())
        assert result is not None
        assert result.type == result_type
        assert body.blocks[0].ops[0].operands == [argument.id]
        assert body.blocks[0].ops[0].results == [result.id]
        verify_module(module, ops=loom.default_ops()).raise_if_errors()
