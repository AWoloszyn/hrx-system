# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Trait derivation contracts for generated operation metadata."""

import pytest

from loom.dsl import DECOMPOSABLE, ELEMENTWISE, VECTOR, Dialect, Op, Operand, Result, SameShape, SameType
from loom.gen.ops.c_metadata_tables import generate_tables_c
from loom.gen.ops.c_traits import _is_shape_preserving_elementwise_vector_decomposable


def test_decomposition_requires_shape_preservation_through_results() -> None:
    for relation in (SameType, SameShape):
        for covers_result in (False, True):
            fields = ("lhs", "rhs", "result") if covers_result else ("lhs", "rhs")
            op = Op(
                "test.elementwise",
                group=Dialect("test"),
                operands=[Operand("lhs", VECTOR), Operand("rhs", VECTOR)],
                results=[Result("result", VECTOR)],
                constraints=[relation(*fields)],
                traits=[ELEMENTWISE],
            )
            assert _is_shape_preserving_elementwise_vector_decomposable(op) == covers_result


def test_explicit_decomposition_accepts_shape_preserving_ops() -> None:
    op = Op(
        "test.elementwise",
        group=Dialect("test"),
        operands=[Operand("input", VECTOR)],
        results=[Result("result", VECTOR)],
        constraints=[SameType("input", "result")],
        traits=[ELEMENTWISE, DECOMPOSABLE],
    )
    generate_tables_c("test", 0, [op])


def test_generate_tables_rejects_explicit_decomposable_for_mixed_result_elementwise_ops() -> None:
    op = Op(
        "test.cmp",
        group=Dialect("test"),
        operands=[Operand("lhs", VECTOR), Operand("rhs", VECTOR)],
        results=[Result("result", VECTOR)],
        constraints=[SameType("lhs", "rhs")],
        traits=[ELEMENTWISE, DECOMPOSABLE],
    )

    with pytest.raises(ValueError, match="Decomposable requires"):
        generate_tables_c("test", 0x01, [op])
