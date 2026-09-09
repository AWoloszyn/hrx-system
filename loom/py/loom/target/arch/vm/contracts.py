# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source operation correspondence for spec-derived VM instructions."""

from iree.vm.bytecode.spec.isa.core.float import (
    FLOAT_CLAMP_SELECTOR,
    FLOAT_COMPARE_SELECTOR,
    FloatBinaryOperation,
    FloatBinarySemantics,
    FloatClampSemantics,
    FloatClassifySemantics,
    FloatCompareSemantics,
    FloatFmaSemantics,
    FloatMinmaxSemantics,
    FloatUnaryOperation,
    FloatUnarySemantics,
)
from iree.vm.bytecode.spec.isa.core.integer import (
    INTEGER_COMPARE_SELECTOR,
    IntegerBinaryOperation,
    IntegerBinarySemantics,
    IntegerCompareSemantics,
    IntegerDivisionOperation,
    IntegerDivisionSemantics,
    IntegerUnaryOperation,
    IntegerUnarySemantics,
)
from iree.vm.bytecode.spec.isa.core.value import VALUE_COPY
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.dialect.scalar import (
    ALL_SCALAR_OPS,
    ClampFMode,
    arithmetic,
    bitwise,
    comparison,
    conversion,
    math,
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
    ternary_descriptor_rules,
    unary_descriptor_rules,
)
from loom.target.low_descriptors import DescriptorOpKind, OperandRole

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

_UNARY_SOURCE_OPS = {
    IntegerUnaryOperation.NEGATE: arithmetic.scalar_negi,
    IntegerUnaryOperation.ABSOLUTE: arithmetic.scalar_absi,
    IntegerUnaryOperation.COUNT_LEADING_ZEROS: bitwise.scalar_ctlzi,
    IntegerUnaryOperation.COUNT_TRAILING_ZEROS: bitwise.scalar_cttzi,
    IntegerUnaryOperation.POPULATION_COUNT: bitwise.scalar_ctpopi,
}

_DIVISION_SOURCE_OPS = {
    IntegerDivisionOperation.SIGNED_QUOTIENT: arithmetic.scalar_divsi,
    IntegerDivisionOperation.UNSIGNED_QUOTIENT: arithmetic.scalar_divui,
    IntegerDivisionOperation.SIGNED_REMAINDER: arithmetic.scalar_remsi,
    IntegerDivisionOperation.UNSIGNED_REMAINDER: arithmetic.scalar_remui,
}

_FLOAT_BINARY_SOURCE_OPS = {
    FloatBinaryOperation.ADD: arithmetic.scalar_addf,
    FloatBinaryOperation.SUB: arithmetic.scalar_subf,
    FloatBinaryOperation.MUL: arithmetic.scalar_mulf,
    FloatBinaryOperation.DIV: arithmetic.scalar_divf,
    FloatBinaryOperation.REM: arithmetic.scalar_remf,
    FloatBinaryOperation.COPY_SIGN: arithmetic.scalar_copysignf,
}

_FLOAT_UNARY_SOURCE_OPS = {
    FloatUnaryOperation.NEGATE: arithmetic.scalar_negf,
    FloatUnaryOperation.ABSOLUTE: arithmetic.scalar_absf,
}

_SELECTED_SOURCE_OPS = {
    FloatMinmaxSemantics: {
        "minimum": arithmetic.scalar_minimumf,
        "maximum": arithmetic.scalar_maximumf,
        "minnum": arithmetic.scalar_minnumf,
        "maxnum": arithmetic.scalar_maxnumf,
    },
    FloatClassifySemantics: {
        "isnan": comparison.scalar_isnanf,
        "isinf": comparison.scalar_isinff,
        "isfinite": comparison.scalar_isfinitef,
    },
}
_ATTRIBUTE_SOURCE_OPS = {
    IntegerCompareSemantics: (comparison.scalar_cmpi, "i", "predicate"),
    FloatCompareSemantics: (comparison.scalar_cmpf, "f", "predicate"),
    FloatClampSemantics: (arithmetic.scalar_clampf, "f", "mode"),
}

# Constants carry bits; the Low result retains the source interpretation.
_CONSTANT_SOURCES = {
    32: {
        "i1": ValueProject.exact_i64,
        "i32": ValueProject.i32_as_u32_bits,
        "f32": ValueProject.float_as_f32_bits,
    },
    64: {
        "i64": ValueProject.exact_i64,
        "f64": ValueProject.float_as_f64_bits,
    },
}

_INSTRUCTIONS = {
    instruction.opcode: instruction for instruction in SPECIFICATION.instructions
}
_DESCRIPTORS = {
    descriptor.encoding_id: descriptor
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors
}

# A direct ordinal projection is valid only while both public enums agree.
for source_enum, selector in (
    (comparison.CmpIPredicate, INTEGER_COMPARE_SELECTOR),
    (comparison.CmpFPredicate, FLOAT_COMPARE_SELECTOR),
    (ClampFMode, FLOAT_CLAMP_SELECTOR),
):
    assert {case.keyword: case.value for case in source_enum.cases} == {
        value.name: value.value for value in selector.values
    }

VM_CORE_CONTRACT_DIALECT_OPS = {"scalar": ALL_SCALAR_OPS}


def _direct_cases(semantics_type, source_ops, type_prefix="i"):
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        semantics = _INSTRUCTIONS[descriptor.encoding_id].semantics
        if isinstance(semantics, semantics_type):
            source_type = Scalar(f"{type_prefix}{semantics.bit_width}")
            if semantics.bit_width == 32 and semantics.operation in (
                IntegerBinaryOperation.AND,
                IntegerBinaryOperation.OR,
                IntegerBinaryOperation.XOR,
            ):
                source_type = Scalar(("i1", "i32"))
            yield DirectDescriptorCase(
                source_ops[semantics.operation],
                descriptor,
                source_type,
            )


def _constant_cases():
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        if descriptor.op_kind is not DescriptorOpKind.CONST:
            continue
        bit_width = descriptor.immediates[0].bit_width
        for source_type, projection in _CONSTANT_SOURCES[bit_width].items():
            yield DescriptorRule(
                source_op=conversion.scalar_constant,
                descriptor=descriptor,
                guards=(Guard.value_type("result", Scalar(source_type)),),
                emit=(
                    EmitDescriptorOp(
                        descriptor=descriptor,
                        results={"destination_v8": ValueRef.result("result")},
                        immediates={"bits": projection("result")},
                        form=DescriptorEmitForm.CONST,
                    ),
                ),
            )


def _selected_rule(descriptor, source_op, source_type, selector):
    result_type = (
        Scalar("i1")
        if isinstance(
            _INSTRUCTIONS[descriptor.encoding_id].semantics,
            (IntegerCompareSemantics, FloatCompareSemantics, FloatClassifySemantics),
        )
        else source_type
    )
    operands = {
        target.field_name: ValueRef.operand(source.name)
        for target, source in zip(
            (
                value
                for value in descriptor.operands
                if value.role is OperandRole.OPERAND
            ),
            source_op.operands,
            strict=True,
        )
    }
    return DescriptorRule(
        source_op=source_op,
        descriptor=descriptor,
        guards=(
            *(
                Guard.value_type(operand.name, source_type)
                for operand in source_op.operands
            ),
            Guard.value_type("result", result_type),
        ),
        emit=(
            EmitDescriptorOp(
                descriptor=descriptor,
                operands=operands,
                results={"destination_v8": ValueRef.result("result")},
                immediates={descriptor.immediates[0].field_name: selector},
            ),
        ),
    )


def _selected_cases():
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        semantics = _INSTRUCTIONS[descriptor.encoding_id].semantics
        if source := _ATTRIBUTE_SOURCE_OPS.get(type(semantics)):
            source_op, type_prefix, attribute = source
            yield _selected_rule(
                descriptor,
                source_op,
                Scalar(f"{type_prefix}{semantics.bit_width}"),
                AttrProject.enum_ordinal(attribute),
            )
        elif source_ops := _SELECTED_SOURCE_OPS.get(type(semantics)):
            (selector,) = (
                field.rule.data
                for field in _INSTRUCTIONS[descriptor.encoding_id].fields
                if field.rule.data is not None
            )
            assert set(source_ops) == {value.name for value in selector.values}
            for value in selector.values:
                yield _selected_rule(
                    descriptor,
                    source_ops[value.name],
                    Scalar(f"f{semantics.bit_width}"),
                    value.value,
                )


VM_CORE_CONTRACT_FRAGMENT = ContractFragment(
    name="vm.core",
    descriptor_set=VM_CORE_DESCRIPTOR_SET,
    public_header="loom/target/arch/vm/contracts/core.h",
    cases=tuple(_constant_cases())
    + tuple(_selected_cases())
    + binary_descriptor_rules(
        tuple(_direct_cases(IntegerBinarySemantics, _BINARY_SOURCE_OPS))
        + tuple(_direct_cases(IntegerDivisionSemantics, _DIVISION_SOURCE_OPS))
        + tuple(_direct_cases(FloatBinarySemantics, _FLOAT_BINARY_SOURCE_OPS, "f")),
        descriptor_result="destination_v8",
        descriptor_lhs="left_v8",
        descriptor_rhs="right_v8",
    )
    + unary_descriptor_rules(
        tuple(_direct_cases(IntegerUnarySemantics, _UNARY_SOURCE_OPS))
        + tuple(_direct_cases(FloatUnarySemantics, _FLOAT_UNARY_SOURCE_OPS, "f"))
        + tuple(
            DirectDescriptorCase(
                conversion.scalar_bitcast,
                _DESCRIPTORS[VALUE_COPY.opcode],
                Scalar(types),
            )
            for types in (("i32", "f32"), ("i64", "f64"))
        ),
        descriptor_result="destination_v8",
        descriptor_input="source_v8",
    )
    + ternary_descriptor_rules(
        tuple(
            DirectDescriptorCase(
                math.scalar_fmaf, descriptor, Scalar(f"f{semantics.bit_width}")
            )
            for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors
            if isinstance(
                semantics := _INSTRUCTIONS[descriptor.encoding_id].semantics,
                FloatFmaSemantics,
            )
        ),
        descriptor_result="destination_v8",
        descriptor_a="a_v8",
        descriptor_b="b_v8",
        descriptor_c="c_v8",
    ),
)
