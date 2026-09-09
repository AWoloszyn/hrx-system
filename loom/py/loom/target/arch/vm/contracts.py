# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source operation correspondence for spec-derived VM instructions."""

from iree.vm.bytecode.spec.isa.core.integer import (
    IntegerBinaryOperation,
    IntegerBinarySemantics,
)
from iree.vm.bytecode.spec.specification import SPECIFICATION

from loom.dialect.scalar import ALL_SCALAR_OPS, arithmetic
from loom.target.arch.vm.descriptors import VM_CORE_DESCRIPTOR_SET
from loom.target.contracts import (
    ContractFragment,
    DirectDescriptorCase,
    Scalar,
    binary_descriptor_rules,
)

_BINARY_SOURCE_OPS = {
    IntegerBinaryOperation.ADD: arithmetic.scalar_addi,
    IntegerBinaryOperation.SUB: arithmetic.scalar_subi,
    IntegerBinaryOperation.MUL: arithmetic.scalar_muli,
}

_INSTRUCTIONS = {
    instruction.opcode: instruction for instruction in SPECIFICATION.instructions
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


VM_CORE_CONTRACT_FRAGMENT = ContractFragment(
    name="vm.core",
    descriptor_set=VM_CORE_DESCRIPTOR_SET,
    public_header="loom/target/arch/vm/contracts/core.h",
    cases=binary_descriptor_rules(
        tuple(_binary_cases()),
        descriptor_result="destination_v8",
        descriptor_lhs="left_v8",
        descriptor_rhs="right_v8",
    ),
)
