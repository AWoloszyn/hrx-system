# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Unsigned constant remainders with a single-LEA quotient product."""

from __future__ import annotations

from collections.abc import Callable

from loom.dialect.index import defs as index
from loom.dialect.scalar import arithmetic as scalar_arithmetic
from loom.dsl import Op
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    GuardDiagnostic,
    Scalar,
    TypePattern,
    ValueProject,
    ValueRef,
)
from loom.target.low_descriptors import Descriptor

_I32 = Scalar("i32")
_I64 = Scalar("i64")
_INDEX = Scalar("index")
_DIVISOR_DIAGNOSTIC = GuardDiagnostic(
    subject_role="divisor",
    subject_name="constant-remainder",
    constraint_key="x86.scalar.remainder_divisor",
)
_NUMERATOR_DIAGNOSTIC = GuardDiagnostic(
    subject_role="numerator",
    subject_name="u32",
    constraint_key="x86.scalar.remainder_u32",
)


def _remainder_rule(
    source_op: Op,
    type_pattern: TypePattern,
    divisor: int,
    descriptor_lookup: Callable[[str], Descriptor],
) -> DescriptorRule:
    register_class = "gpr32" if type_pattern == _I32 else "gpr64"
    subtract = descriptor_lookup(f"x86.scalar.sub.{register_class}")
    numerator = ValueRef.operand("lhs")
    emits = []
    if type_pattern == _I32:
        numerator = ValueRef.temporary("numerator")
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.movzx.gpr64.gpr32"),
                operands={"src": ValueRef.operand("lhs")},
                results={"dst": numerator},
                result_types={"dst": _I64},
            )
        )
    emits.extend(
        (
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.movimm.gpr64"),
                results={"dst": ValueRef.temporary("magic")},
                result_types={"dst": _I64},
                immediates={"imm64": ValueProject.u32_divisor_magic_multiplier("rhs")},
                form=DescriptorEmitForm.CONST,
            ),
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.imul.gpr64"),
                operands={"lhs": numerator, "rhs": ValueRef.temporary("magic")},
                results={"dst": ValueRef.temporary("wide_product")},
                result_types={"dst": _I64},
            ),
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.shr.imm.gpr64"),
                operands={"lhs": ValueRef.temporary("wide_product")},
                results={"dst": ValueRef.temporary("quotient")},
                result_types={"dst": _I64},
                immediates={
                    "shift": ValueProject.u32_divisor_magic_shift(
                        "rhs", product_bit_width=64
                    )
                },
            ),
            EmitDescriptorOp(
                descriptor=descriptor_lookup(
                    f"x86.scalar.lea.add_scale.{register_class}"
                ),
                operands={
                    "base": ValueRef.temporary("quotient"),
                    "index": ValueRef.temporary("quotient"),
                },
                results={"dst": ValueRef.temporary("quotient_product")},
                result_types={"dst": type_pattern},
                immediates={"disp32": 0, "scale": divisor - 1},
            ),
            EmitDescriptorOp(
                descriptor=subtract,
                operands={
                    "lhs": ValueRef.operand("lhs"),
                    "rhs": ValueRef.temporary("quotient_product"),
                },
                results={"dst": ValueRef.result("result")},
            ),
        )
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=subtract,
        guards=(
            *(
                Guard.value_type(field, type_pattern)
                for field in ("lhs", "rhs", "result")
            ),
            *(
                (
                    Guard.value_unsigned_bit_count(
                        "lhs", 32, diagnostic=_NUMERATOR_DIAGNOSTIC
                    ),
                )
                if type_pattern == _INDEX
                else ()
            ),
            Guard.value_exact_i64("rhs", diagnostic=_DIVISOR_DIAGNOSTIC),
            Guard.value_i64_range(
                "rhs", divisor, divisor, diagnostic=_DIVISOR_DIAGNOSTIC
            ),
        ),
        emit=tuple(emits),
    )


def unsigned_remainder_rules(
    descriptor_lookup: Callable[[str], Descriptor],
) -> tuple[DescriptorRule, ...]:
    return tuple(
        _remainder_rule(source_op, type_pattern, divisor, descriptor_lookup)
        for source_op, type_pattern in (
            (scalar_arithmetic.scalar_remui, _I32),
            (index.index_rem, _INDEX),
        )
        for divisor in (3,)
    )
