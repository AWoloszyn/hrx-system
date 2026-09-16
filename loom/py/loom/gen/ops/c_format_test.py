# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.assembly import Clause, OptionalGroup, Ref, Refs
from loom.dsl import INTEGER, Dialect, Op, Operand
from loom.gen.ops.c_tables import generate_tables_c


def _assert_invalid_format(op: Op, expected: str) -> None:
    try:
        generate_tables_c("test", 0, [op])
    except ValueError as error:
        if expected not in str(error):
            raise AssertionError(f"{error!s} does not contain {expected!r}") from error
    else:
        raise AssertionError(f"expected invalid format: {expected}")


def test_optional_clauses_follow_declaration_order() -> None:
    operands = [
        Operand("values", INTEGER, variadic=True),
        Operand("depth", INTEGER, optional=True),
        Operand("factor", INTEGER, optional=True),
    ]
    depth = OptionalGroup([Clause("pipeline", Ref("depth"))], anchor="depth")
    factor = OptionalGroup([Clause("unroll", Ref("factor"))], anchor="factor")
    for clauses in ([depth, factor], [factor, depth]):
        op = Op(
            "test.policies",
            group=Dialect("test"),
            operands=operands,
            format=[Refs("values"), *clauses],
        )
        if clauses[0] is depth:
            generate_tables_c("test", 0, [op])
        else:
            _assert_invalid_format(op, "segmented operands must appear in declaration order; 'depth' follows 'factor'")


def test_variadic_groups_follow_declaration_order() -> None:
    op = Op(
        "test.groups",
        group=Dialect("test"),
        operands=[
            Operand("first", INTEGER, variadic=True),
            Operand("second", INTEGER, variadic=True),
        ],
        format=[Refs("second"), Refs("first")],
    )
    _assert_invalid_format(op, "segmented operands must appear in declaration order; 'first' follows 'second'")


def test_fixed_operands_can_use_format_order() -> None:
    op = Op(
        "test.fixed",
        group=Dialect("test"),
        operands=[Operand("first", INTEGER), Operand("second", INTEGER)],
        format=[Ref("second"), Ref("first")],
    )
    generate_tables_c("test", 0, [op])
