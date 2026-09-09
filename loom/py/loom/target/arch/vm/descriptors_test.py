# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from iree.vm.bytecode.spec.isa import ControlFlow, FieldRole, Suspension
from iree.vm.bytecode.spec.isa.core.integer import IntegerBinarySemantics
from iree.vm.bytecode.spec.isa.core.rules import FieldRule
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.ir import ScalarTypeKind
from loom.target.arch.vm.contracts import VM_CORE_CONTRACT_FRAGMENT
from loom.target.arch.vm.descriptors import VM_CORE_DESCRIPTOR_SET
from loom.target.low_descriptors import DescriptorFlag, OperandRole


def test_integer_packets_preserve_spec_encoding_and_semantic_types():
    descriptors = {
        descriptor.encoding_id: descriptor
        for descriptor in VM_CORE_DESCRIPTOR_SET.descriptors
    }
    instructions = [
        instruction
        for instruction in SPECIFICATION.instructions
        if isinstance(instruction.semantics, IntegerBinarySemantics)
    ]
    assert instructions
    assert len(descriptors) == len(instructions)
    for instruction in instructions:
        descriptor = descriptors[instruction.opcode]
        assert descriptor.mnemonic == instruction.mnemonic
        assert instruction.control_flow is ControlFlow.SEQUENTIAL
        assert instruction.suspension is Suspension.NEVER
        assert not instruction.state_effects
        assert not instruction.failures
        assert DescriptorFlag.DEAD_REMOVABLE in descriptor.flags
        assert (
            tuple(operand.encoding_field_id for operand in descriptor.operands)
            == instruction.field_offsets
        )
        for field, operand in zip(instruction.fields, descriptor.operands, strict=True):
            assert field.rule.kind is FieldRule.REGISTER_VALUE
            assert field.field.name == operand.field_name
            assert (field.role is FieldRole.RESULT) == (
                operand.role is OperandRole.RESULT
            )
        result_type = descriptor.asm_forms[0].result_value_types[0]
        assert (
            result_type.element_type
            is {
                32: ScalarTypeKind.I32,
                64: ScalarTypeKind.I64,
            }[instruction.semantics.bit_width]
        )


def test_lowering_uses_the_projected_descriptors():
    descriptors = VM_CORE_DESCRIPTOR_SET.descriptors
    assert {id(case.descriptor) for case in VM_CORE_CONTRACT_FRAGMENT.cases} == {
        id(descriptor) for descriptor in descriptors
    }
    for case in VM_CORE_CONTRACT_FRAGMENT.cases:
        emit = case.emit[0]
        assert emit.descriptor is case.descriptor
        assert set(emit.operands) == {"left_v8", "right_v8"}
        assert set(emit.results) == {"destination_v8"}
