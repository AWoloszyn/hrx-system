# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from iree.vm.bytecode.spec.isa import ControlFlow, FieldRole, Suspension
from iree.vm.bytecode.spec.isa.core.constant import CONSTANT_I32, CONSTANT_I64
from iree.vm.bytecode.spec.isa.core.float import (
    FloatBinarySemantics,
    FloatClampSemantics,
    FloatClassifySemantics,
    FloatCompareSemantics,
    FloatFmaSemantics,
    FloatMinmaxSemantics,
    FloatUnarySemantics,
)
from iree.vm.bytecode.spec.isa.core.integer import (
    IntegerBinarySemantics,
    IntegerCompareSemantics,
    IntegerDivisionSemantics,
    IntegerUnarySemantics,
)
from iree.vm.bytecode.spec.isa.core.rules import FieldRule
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.ir import ScalarTypeKind
from loom.target.arch.vm.contracts import VM_CORE_CONTRACT_FRAGMENT
from loom.target.arch.vm.descriptors import VM_CORE_DESCRIPTOR_SET
from loom.target.low_descriptors import (
    DescriptorFlag,
    DescriptorOpKind,
    EffectKind,
    OperandRole,
)


def test_scalar_packets_preserve_spec_encoding_and_semantic_types():
    descriptors = {
        descriptor.encoding_id: descriptor
        for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors
    }
    instructions = [
        instruction
        for instruction in SPECIFICATION.instructions
        if isinstance(
            instruction.semantics,
            (
                IntegerBinarySemantics,
                IntegerUnarySemantics,
                IntegerCompareSemantics,
                IntegerDivisionSemantics,
                FloatBinarySemantics,
                FloatUnarySemantics,
                FloatMinmaxSemantics,
                FloatCompareSemantics,
                FloatClassifySemantics,
                FloatClampSemantics,
                FloatFmaSemantics,
            ),
        )
    ]
    assert instructions
    for instruction in instructions:
        descriptor = descriptors[instruction.opcode]
        assert descriptor.mnemonic == instruction.mnemonic
        assert descriptor.encoding_format_id == instruction.byte_length
        assert instruction.control_flow is ControlFlow.SEQUENTIAL
        assert instruction.suspension is Suspension.NEVER
        assert not instruction.state_effects
        register_fields = tuple(
            (field, offset)
            for field, offset in zip(
                instruction.fields, instruction.field_offsets, strict=True
            )
            if field.role in (FieldRole.RESULT, FieldRole.OPERAND)
        )
        for (field, offset), operand in zip(
            register_fields, descriptor.operands, strict=True
        ):
            assert operand.encoding_field_id == offset
            assert field.rule.kind is FieldRule.REGISTER_VALUE
            assert field.field.name == operand.field_name
            assert (field.role is FieldRole.RESULT) == (
                operand.role is OperandRole.RESULT
            )
        result_type = descriptor.asm_forms[0].result_value_types[0]
        scalar_types = (
            {32: ScalarTypeKind.F32, 64: ScalarTypeKind.F64}
            if isinstance(
                instruction.semantics,
                (
                    FloatBinarySemantics,
                    FloatUnarySemantics,
                    FloatMinmaxSemantics,
                    FloatClampSemantics,
                    FloatFmaSemantics,
                ),
            )
            else {32: ScalarTypeKind.I32, 64: ScalarTypeKind.I64}
        )
        expected_type = (
            ScalarTypeKind.I1
            if isinstance(
                instruction.semantics,
                (
                    IntegerCompareSemantics,
                    FloatCompareSemantics,
                    FloatClassifySemantics,
                ),
            )
            else scalar_types[instruction.semantics.bit_width]
        )
        assert result_type.element_type is expected_type


def test_scalar_effects_preserve_observable_failures():
    instructions = {
        instruction.opcode: instruction for instruction in SPECIFICATION.instructions
    }
    for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors:
        instruction = instructions[descriptor.encoding_id]
        if instruction.failures:
            assert DescriptorFlag.SIDE_EFFECTING in descriptor.flags
            assert DescriptorFlag.DEAD_REMOVABLE not in descriptor.flags
            assert tuple(effect.kind for effect in descriptor.effects) == (
                EffectKind.FAILURE,
            )
        else:
            assert not descriptor.effects
            assert DescriptorFlag.DEAD_REMOVABLE in descriptor.flags


def test_lowering_uses_the_projected_descriptors():
    descriptors = VM_CORE_DESCRIPTOR_SET.descriptors
    cases = VM_CORE_CONTRACT_FRAGMENT.cases
    assert {id(case.descriptor) for case in cases} == {
        id(descriptor) for descriptor in descriptors
    }
    for case in cases:
        emit = case.emit[0]
        assert emit.descriptor is case.descriptor
        assert set(emit.operands) == {
            operand.field_name
            for operand in case.descriptor.operands
            if operand.role is OperandRole.OPERAND
        }
        assert set(emit.results) == {
            operand.field_name
            for operand in case.descriptor.operands
            if operand.role is OperandRole.RESULT
        }


def test_selectors_preserve_the_spec_domain_and_encoding():
    descriptors = {
        descriptor.encoding_id: descriptor
        for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors
    }
    domains = {domain.name: domain for domain in VM_CORE_DESCRIPTOR_SET.enum_domains}
    for instruction in SPECIFICATION.instructions:
        if instruction.opcode not in descriptors:
            continue
        descriptor = descriptors[instruction.opcode]
        if not descriptor.immediates or descriptor.op_kind is DescriptorOpKind.CONST:
            continue
        (immediate,) = descriptor.immediates
        (selector,) = (
            (field, offset)
            for field, offset in zip(
                instruction.fields, instruction.field_offsets, strict=True
            )
            if field.rule.kind is FieldRule.SELECTOR
        )
        field, offset = selector
        assert immediate.encoding_field_id == offset
        assert immediate.bit_width == field.field.byte_length * 8
        assert {
            entry.token: entry.value for entry in domains[immediate.enum_domain].values
        } == {entry.name: entry.value for entry in field.rule.data.values}


def test_constant_immediates_preserve_the_wire_bits_and_alignment():
    descriptors = {
        descriptor.encoding_id: descriptor
        for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors
    }
    for instruction in (CONSTANT_I32, CONSTANT_I64):
        descriptor = descriptors[instruction.opcode]
        assert descriptor.op_kind is DescriptorOpKind.CONST
        assert descriptor.encoding_format_id == instruction.byte_length
        (immediate,) = descriptor.immediates
        fields = tuple(
            (field.field, offset)
            for field, offset in zip(
                instruction.fields, instruction.field_offsets, strict=True
            )
            if field.role is FieldRole.IMMEDIATE
        )
        assert immediate.encoding_field_id == fields[0][1]
        assert immediate.bit_width == 8 * sum(field.byte_length for field, _ in fields)
        # One contiguous logical value must preserve each naturally aligned word,
        # including the sign bit and nonzero high half of a wide constant.
        for bits in (0, 0xFEDCBA98, 0x81234567FEDCBA98, 0xFFFFFFFFFFFFFFFF):
            value = bits & ((1 << immediate.bit_width) - 1)
            encoded = value.to_bytes(immediate.bit_width // 8, "little")
            for field, offset in fields:
                relative_offset = offset - immediate.encoding_field_id
                assert offset % field.encoding.alignment == 0
                assert (
                    int.from_bytes(
                        encoded[relative_offset : relative_offset + field.byte_length],
                        "little",
                    )
                    == (value >> (relative_offset * 8)) & 0xFFFFFFFF
                )
