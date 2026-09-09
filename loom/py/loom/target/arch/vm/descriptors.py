# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Projects VM instruction records into Loom's shared Low descriptor schema.

The runtime spec owns opcodes, packet fields, and semantics. This projection
supplies the interpreter's register and scheduling model, not a second ISA.
Encoding IDs are byte opcodes, encoding format IDs are packet byte lengths,
and operand encoding field IDs are byte offsets from the instruction start.
"""

from pathlib import Path

from iree.vm.bytecode.spec.isa import FieldRole, Instruction
from iree.vm.bytecode.spec.isa.core.constant import CONSTANT_I32, CONSTANT_I64
from iree.vm.bytecode.spec.isa.core.conversion import (
    CONVERSION_INSTRUCTIONS,
    FLOAT_EXTEND_SELECTOR,
    FLOAT_TO_INTEGER_SELECTOR,
    FLOAT_TRUNCATE_SELECTOR,
    FLOAT_WIDTH_SELECTOR,
    INTEGER_CONVERT_SELECTOR,
    INTEGER_TO_FLOAT_SELECTOR,
)
from iree.vm.bytecode.spec.isa.core.float import (
    FloatBinarySemantics,
    FloatClampSemantics,
    FloatClassifySemantics,
    FloatCompareSemantics,
    FloatFmaSemantics,
    FloatMathSemantics,
    FloatMinmaxSemantics,
    FloatUnarySemantics,
)
from iree.vm.bytecode.spec.isa.core.integer import (
    IntegerBinarySemantics,
    IntegerCompareSemantics,
    IntegerDivisionSemantics,
    IntegerUnarySemantics,
)
from iree.vm.bytecode.spec.isa.core.value import VALUE_COPY, VALUE_SELECT
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.ir import ScalarTypeKind
from loom.target.low_descriptors import (
    AsmForm,
    AsmImmediate,
    AsmResultValueType,
    Descriptor,
    DescriptorFlag,
    DescriptorOpKind,
    DescriptorSet,
    Effect,
    EffectKind,
    EnumDomain,
    EnumValue,
    Immediate,
    ImmediateKind,
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

_INTEGER_TYPES = {32: ScalarTypeKind.I32, 64: ScalarTypeKind.I64}
_FLOAT_TYPES = {32: ScalarTypeKind.F32, 64: ScalarTypeKind.F64}
_PREDICATE_TYPES = {32: ScalarTypeKind.I1, 64: ScalarTypeKind.I1}
_RESULT_TYPES = {
    IntegerBinarySemantics: _INTEGER_TYPES,
    IntegerUnarySemantics: _INTEGER_TYPES,
    IntegerCompareSemantics: _PREDICATE_TYPES,
    IntegerDivisionSemantics: _INTEGER_TYPES,
    FloatBinarySemantics: _FLOAT_TYPES,
    FloatUnarySemantics: _FLOAT_TYPES,
    FloatMinmaxSemantics: _FLOAT_TYPES,
    FloatCompareSemantics: _PREDICATE_TYPES,
    FloatClassifySemantics: _PREDICATE_TYPES,
    FloatClampSemantics: _FLOAT_TYPES,
    FloatFmaSemantics: _FLOAT_TYPES,
    FloatMathSemantics: _FLOAT_TYPES,
}
_SCALAR_INSTRUCTIONS = tuple(
    instruction
    for instruction in SPECIFICATION.instructions
    if type(instruction.semantics) in _RESULT_TYPES
)
_SCALAR_CONVERSIONS = tuple(
    instruction
    for instruction in CONVERSION_INSTRUCTIONS
    if instruction.fields[-1].rule.data
    in (
        INTEGER_CONVERT_SELECTOR,
        FLOAT_EXTEND_SELECTOR,
        FLOAT_TRUNCATE_SELECTOR,
        FLOAT_WIDTH_SELECTOR,
        INTEGER_TO_FLOAT_SELECTOR,
        FLOAT_TO_INTEGER_SELECTOR,
    )
)
_SELECTORS = {
    field.rule.data.name: field.rule.data
    for instruction in (*_SCALAR_INSTRUCTIONS, *_SCALAR_CONVERSIONS)
    for field in instruction.fields
    if field.role is FieldRole.IMMEDIATE
}


def _selector_immediates(instruction: Instruction) -> tuple[Immediate, ...]:
    return tuple(
        Immediate(
            field.field.name,
            ImmediateKind.ENUM,
            bit_width=field.field.byte_length * 8,
            encoding_field_id=offset,
            enum_domain=field.rule.data.name,
        )
        for field, offset in zip(
            instruction.fields, instruction.field_offsets, strict=True
        )
        if field.role is FieldRole.IMMEDIATE
    )


def _value_descriptor(
    instruction: Instruction,
    result_type: ScalarTypeKind,
    *,
    op_kind: DescriptorOpKind = DescriptorOpKind.OP,
    immediates: tuple[Immediate, ...] | None = None,
) -> Descriptor:
    if immediates is None:
        immediates = _selector_immediates(instruction)
    # The emitter has a bounded packet and positional storage for one immediate.
    assert instruction.byte_length <= CONSTANT_I64.byte_length
    assert len(immediates) <= 1
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
        if field.role in _OPERAND_ROLES
    )
    return Descriptor(
        key=f"vm.{instruction.mnemonic}",
        mnemonic=instruction.mnemonic,
        semantic_tag=None,
        operands=operands,
        schedule_class="vm.scalar",
        op_kind=op_kind,
        immediates=immediates,
        encoding_id=instruction.opcode,
        encoding_format_id=instruction.byte_length,
        effects=(Effect(EffectKind.FAILURE),) if instruction.failures else (),
        flags=(
            DescriptorFlag.SIDE_EFFECTING
            if instruction.failures
            else DescriptorFlag.DEAD_REMOVABLE,
        ),
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
                immediates=tuple(
                    AsmImmediate(value.field_name) for value in immediates
                ),
                result_value_types=(AsmResultValueType(result_type),),
            ),
        ),
    )


def _constant_descriptor(instruction: Instruction) -> Descriptor:
    # Loom carries the exact scalar bits as one immediate. The wire stores wide
    # constants as consecutive u32 words to preserve instruction alignment.
    fields = tuple(
        (field.field, offset)
        for field, offset in zip(
            instruction.fields, instruction.field_offsets, strict=True
        )
        if field.role is FieldRole.IMMEDIATE
    )
    offset = fields[0][1]
    byte_length = sum(field.byte_length for field, _ in fields)
    assert tuple(position for _, position in fields) == tuple(
        range(offset, offset + byte_length, 4)
    )
    bit_width = byte_length * 8
    return _value_descriptor(
        instruction,
        {32: ScalarTypeKind.I32, 64: ScalarTypeKind.I64}[bit_width],
        op_kind=DescriptorOpKind.CONST,
        immediates=(
            Immediate(
                "bits",
                ImmediateKind.UNSIGNED if bit_width == 32 else ImmediateKind.SIGNED,
                bit_width=bit_width,
                encoding_field_id=offset,
                signed_min=-(1 << 63) if bit_width == 64 else 0,
                unsigned_max=(1 << bit_width) - 1,
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
    enum_domains=tuple(
        EnumDomain(
            selector.name,
            tuple(EnumValue(value.name, value.value) for value in selector.values),
        )
        for selector in _SELECTORS.values()
    ),
    descriptors=(
        _value_descriptor(VALUE_COPY, ScalarTypeKind.I64),
        _value_descriptor(VALUE_SELECT, ScalarTypeKind.I64),
        *(_constant_descriptor(op) for op in (CONSTANT_I32, CONSTANT_I64)),
        *(_value_descriptor(op, ScalarTypeKind.I64) for op in _SCALAR_CONVERSIONS),
        *(
            _value_descriptor(
                instruction,
                _RESULT_TYPES[type(instruction.semantics)][
                    instruction.semantics.bit_width
                ],
            )
            for instruction in _SCALAR_INSTRUCTIONS
        ),
    ),
)
