# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source operation correspondence for spec-derived VM instructions."""

from iree.vm.bytecode.spec.isa.core.integer import (
    INTEGER_COMPARE_SELECTOR,
    IntegerBinaryOperation,
    IntegerBinarySemantics,
    IntegerCompareSemantics,
)
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.dialect.scalar import (
    ALL_SCALAR_OPS,
    arithmetic,
    bitwise,
    comparison,
    conversion,
)
from loom.target.arch.vm.descriptors import VM_CORE_DESCRIPTOR_SET
from loom.target.contracts import (
    AttrProject,
    ContractFragment,
    DescriptorEmitForm,
    DescriptorRule,
    DirectDescriptorCase,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueProject,
    ValueRef,
    binary_descriptor_rules,
)
from loom.target.low_descriptors import DescriptorOpKind

_BINARY_SOURCE_OPS = {
    IntegerBinaryOperation.ADD: arithmetic.scalar_addi,
    IntegerBinaryOperation.SUB: arithmetic.scalar_subi,
    IntegerBinaryOperation.MUL: arithmetic.scalar_muli,
    IntegerBinaryOperation.MIN_SIGNED: arithmetic.scalar_minsi,
    IntegerBinaryOperation.MIN_UNSIGNED: arithmetic.scalar_minui,
    IntegerBinaryOperation.MAX_SIGNED: arithmetic.scalar_maxsi,
    IntegerBinaryOperation.MAX_UNSIGNED: arithmetic.scalar_maxui,
    IntegerBinaryOperation.AND: bitwise.scalar_andi,
    IntegerBinaryOperation.OR: bitwise.scalar_ori,
    IntegerBinaryOperation.XOR: bitwise.scalar_xori,
    IntegerBinaryOperation.SHIFT_LEFT: bitwise.scalar_shli,
    IntegerBinaryOperation.SHIFT_RIGHT_SIGNED: bitwise.scalar_shrsi,
    IntegerBinaryOperation.SHIFT_RIGHT_UNSIGNED: bitwise.scalar_shrui,
    IntegerBinaryOperation.ROTATE_LEFT: bitwise.scalar_rotli,
    IntegerBinaryOperation.ROTATE_RIGHT: bitwise.scalar_rotri,
}

_INSTRUCTIONS = {
    instruction.opcode: instruction for instruction in SPECIFICATION.instructions
}

# A direct ordinal projection is valid only while both public enums agree.
assert {case.keyword: case.value for case in comparison.CmpIPredicate.cases} == {
    value.name: value.value for value in INTEGER_COMPARE_SELECTOR.values
}

VM_CORE_CONTRACT_DIALECT_OPS = {"scalar": ALL_SCALAR_OPS}


def _binary_cases():
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        semantics = _INSTRUCTIONS[descriptor.encoding_id].semantics
        if isinstance(semantics, IntegerBinarySemantics):
            yield DirectDescriptorCase(
                _BINARY_SOURCE_OPS[semantics.operation],
                descriptor,
                Scalar(f"i{semantics.bit_width}"),
            )


def _constant_cases():
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        if descriptor.op_kind is not DescriptorOpKind.CONST:
            continue
        bit_width = descriptor.immediates[0].bit_width
        yield DescriptorRule(
            source_op=conversion.scalar_constant,
            descriptor=descriptor,
            guards=(Guard.value_type("result", Scalar(f"i{bit_width}")),),
            emit=(
                EmitDescriptorOp(
                    descriptor=descriptor,
                    results={"destination_v8": ValueRef.result("result")},
                    immediates={
                        "bits": ValueProject.i32_as_u32_bits("result")
                        if bit_width == 32
                        else ValueProject.exact_i64("result")
                    },
                    form=DescriptorEmitForm.CONST,
                ),
            ),
        )


def _compare_cases():
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        semantics = _INSTRUCTIONS[descriptor.encoding_id].semantics
        if not isinstance(semantics, IntegerCompareSemantics):
            continue
        operand_type = Scalar(f"i{semantics.bit_width}")
        yield DescriptorRule(
            source_op=comparison.scalar_cmpi,
            descriptor=descriptor,
            guards=(
                Guard.value_type("lhs", operand_type),
                Guard.value_type("rhs", operand_type),
                Guard.value_type("result", Scalar("i1")),
            ),
            emit=(
                EmitDescriptorOp(
                    descriptor=descriptor,
                    operands={
                        "left_v8": ValueRef.operand("lhs"),
                        "right_v8": ValueRef.operand("rhs"),
                    },
                    results={"destination_v8": ValueRef.result("result")},
                    immediates={
                        descriptor.immediates[0].field_name: AttrProject.enum_ordinal(
                            "predicate"
                        )
                    },
                ),
            ),
        )


VM_CORE_CONTRACT_FRAGMENT = ContractFragment(
    name="vm.core",
    descriptor_set=VM_CORE_DESCRIPTOR_SET,
    public_header="loom/target/arch/vm/contracts/core.h",
    cases=tuple(_constant_cases())
    + tuple(_compare_cases())
    + binary_descriptor_rules(
        tuple(_binary_cases()),
        descriptor_result="destination_v8",
        descriptor_lhs="left_v8",
        descriptor_rhs="right_v8",
    ),
)
