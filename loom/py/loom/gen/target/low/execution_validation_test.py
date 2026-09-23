# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Execution and predication obligations at descriptor construction."""

from dataclasses import replace

import pytest

from loom.gen.target.low import validation
from loom.target.low_descriptors import (
    DescriptorFlag,
    Effect,
    EffectKind,
    MemorySpace,
    Operand,
    OperandFlag,
    OperandRole,
    RegClassAlt,
)
from loom.target.test.descriptors import TEST_LOW_ADD_I32_DESCRIPTOR

_TOTAL_ADD = replace(
    TEST_LOW_ADD_I32_DESCRIPTOR,
    flags=(DescriptorFlag.DEAD_REMOVABLE, DescriptorFlag.SAFE_TO_SPECULATE),
)
_MASK = Operand(
    "active",
    OperandRole.IMPLICIT,
    (RegClassAlt("test.mask"),),
    flags=(OperandFlag.IMPLICIT, OperandFlag.STATE_READ, OperandFlag.SCHEDULE_ONLY_STATE, OperandFlag.EXECUTION_MASK),
)


def test_total_arithmetic_can_read_a_lane_mask_or_unchanged_state() -> None:
    validation.validate_descriptor_speculation(_TOTAL_ADD)
    for state in (_MASK, replace(_MASK, flags=(OperandFlag.IMPLICIT, OperandFlag.STATE_READ))):
        descriptor = replace(_TOTAL_ADD, operands=(*_TOTAL_ADD.operands, state))
        validation.validate_descriptor_operands(descriptor)
        validation.validate_descriptor_speculation(descriptor)


@pytest.mark.parametrize("kind", [EffectKind.READ, EffectKind.WRITE, EffectKind.CONVERGENT])
def test_speculation_rejects_observable_or_collective_effects(kind: EffectKind) -> None:
    descriptor = replace(_TOTAL_ADD, effects=(Effect(kind, MemorySpace.GLOBAL),))
    with pytest.raises(ValueError, match="effect-free"):
        validation.validate_descriptor_speculation(descriptor)


def test_speculation_rejects_hidden_flag_writes() -> None:
    state = replace(_MASK, flags=(OperandFlag.IMPLICIT, OperandFlag.STATE_WRITE))
    descriptor = replace(_TOTAL_ADD, operands=(*_TOTAL_ADD.operands, state))
    with pytest.raises(ValueError, match="cannot write architectural state"):
        validation.validate_descriptor_speculation(descriptor)


def test_mask_narrowing_requires_the_incoming_mask() -> None:
    output = replace(_MASK, field_name="next_active", flags=(OperandFlag.IMPLICIT, OperandFlag.STATE_WRITE, OperandFlag.NARROWS_EXECUTION_MASK))
    descriptor = replace(TEST_LOW_ADD_I32_DESCRIPTOR, operands=(*TEST_LOW_ADD_I32_DESCRIPTOR.operands, output))
    with pytest.raises(ValueError, match="must read the same execution mask"):
        validation.validate_descriptor_operands(descriptor)
    validation.validate_descriptor_operands(replace(descriptor, operands=(*descriptor.operands, _MASK)))


def test_execution_mask_is_not_an_arbitrary_state_annotation() -> None:
    state = replace(_MASK, flags=(OperandFlag.IMPLICIT, OperandFlag.STATE_WRITE, OperandFlag.EXECUTION_MASK))
    descriptor = replace(TEST_LOW_ADD_I32_DESCRIPTOR, operands=(*TEST_LOW_ADD_I32_DESCRIPTOR.operands, state))
    with pytest.raises(ValueError, match="implicit schedule-only state read"):
        validation.validate_descriptor_operands(descriptor)
