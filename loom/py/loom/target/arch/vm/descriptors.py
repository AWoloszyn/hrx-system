# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Projects VM instruction records into Loom's shared Low descriptor schema.

The runtime spec owns opcodes, packet fields, and semantics. This projection
supplies the interpreter's register and scheduling model, not a second ISA.
Encoding IDs are byte opcodes and operand encoding field IDs are byte offsets
from the beginning of the instruction, including its opcode byte.
"""

from pathlib import Path

from iree.vm.bytecode.spec.isa import FieldRole, Instruction
from iree.vm.bytecode.spec.isa.core.integer import IntegerBinarySemantics
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.ir import ScalarTypeKind
from loom.target.low_descriptors import (
    AsmForm,
    AsmResultValueType,
    Descriptor,
    DescriptorFlag,
    DescriptorSet,
    InstructionClass,
    IssueUse,
    LatencyKind,
    ModelQuality,
    Operand,
    OperandRole,
    RegClass,
    RegClassAlt,
    RegClassFlag,
    Resource,
    ResourceKind,
    ScheduleClass,
    SpillSlotSpace,
)

_OPERAND_ROLES = {
    FieldRole.RESULT: OperandRole.RESULT,
    FieldRole.OPERAND: OperandRole.OPERAND,
}


def _integer_binary_descriptor(instruction: Instruction) -> Descriptor:
    semantics = instruction.semantics
    assert isinstance(semantics, IntegerBinarySemantics)
    operands = tuple(
        Operand(
            field.field.name,
            _OPERAND_ROLES[field.role],
            (RegClassAlt("vm.value"),),
            encoding_field_id=offset,
        )
        for field, offset in zip(
            instruction.fields, instruction.field_offsets, strict=True
        )
    )
    return Descriptor(
        key=f"vm.{instruction.mnemonic}",
        mnemonic=instruction.mnemonic,
        semantic_tag=None,
        operands=operands,
        schedule_class="vm.scalar",
        encoding_id=instruction.opcode,
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
        instruction_classes=(InstructionClass.SCALAR_ALU,),
        asm_forms=(
            AsmForm(
                results=tuple(
                    operand.field_name
                    for operand in operands
                    if operand.role is OperandRole.RESULT
                ),
                operands=tuple(
                    operand.field_name
                    for operand in operands
                    if operand.role is OperandRole.OPERAND
                ),
                result_value_types=(
                    AsmResultValueType(
                        {32: ScalarTypeKind.I32, 64: ScalarTypeKind.I64}[
                            semantics.bit_width
                        ]
                    ),
                ),
            ),
        ),
    )


VM_CORE_DESCRIPTOR_SET = DescriptorSet(
    key="vm.core",
    target_key="vm",
    feature_key=None,
    c_header_path=Path("loom/src/loom/target/arch/vm/descriptors/descriptors.h"),
    c_source_path=Path("loom/src/loom/target/arch/vm/descriptors/descriptors.c"),
    header_guard="LOOM_TARGET_ARCH_VM_DESCRIPTORS_DESCRIPTORS_H_",
    public_header="loom/target/arch/vm/descriptors/descriptors.h",
    function_name="loom_vm_core_descriptor_set",
    c_table_prefix="VmCore",
    c_enum_prefix="VM_CORE",
    generator_version=1,
    reg_classes=(
        RegClass(
            "vm.value",
            alloc_unit_bits=64,
            spill_slot_space=SpillSlotSpace.STACK,
            flags=(RegClassFlag.PHYSICAL,),
            allocatable_count=256,
        ),
    ),
    resources=(Resource("vm.issue", 1, ResourceKind.SCALAR_ALU),),
    schedule_classes=(
        ScheduleClass(
            "vm.scalar",
            latency_kind=LatencyKind.ESTIMATE,
            model_quality=ModelQuality.ESTIMATED,
            latency_cycles=1,
            issue_uses=(IssueUse("vm.issue", cycles=1, units=1),),
        ),
    ),
    requires_explicit_asm_surface=True,
    descriptors=tuple(
        _integer_binary_descriptor(instruction)
        for instruction in SPECIFICATION.instructions
        if isinstance(instruction.semantics, IntegerBinarySemantics)
    ),
)
