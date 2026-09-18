# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AMDGPU register dependency timing."""

from __future__ import annotations

from dataclasses import replace

from loom.target.low_descriptors import (
    Descriptor,
    DescriptorSet,
    EventSeparation,
    ModelQuality,
    Operand,
    OperandFlag,
    OperandRole,
    TimingEvent,
)

from .common import _REG_SGPR, _SCHEDULE_VALU


def _with_valu_sgpr_timing(
    descriptor_set: DescriptorSet, separation_cycles: int
) -> DescriptorSet:
    # Compare masks and lane reads are produced by VALU into scalar registers.
    # Model their availability at operand endpoints so independent instructions
    # can fill the dependency gap. Class latency also controls completion and
    # critical-path priority, which have different scheduling consequences.
    read_event = "amdgpu.sgpr.read"
    write_event = "amdgpu.valu.sgpr.write"
    descriptors: list[Descriptor] = []
    for descriptor in descriptor_set.descriptors:
        operands: list[Operand] = []
        for operand in descriptor.operands:
            has_sgpr = any(
                alternative.reg_class == _REG_SGPR for alternative in operand.reg_alts
            )
            reads_sgpr = has_sgpr and (
                operand.role
                in (
                    OperandRole.OPERAND,
                    OperandRole.OPERAND_RESULT,
                    OperandRole.PREDICATE,
                    OperandRole.RESOURCE,
                )
                or OperandFlag.STATE_READ in operand.flags
            )
            writes_sgpr = (
                has_sgpr
                and descriptor.schedule_class == _SCHEDULE_VALU
                and (
                    operand.role in (OperandRole.RESULT, OperandRole.OPERAND_RESULT)
                    or OperandFlag.STATE_WRITE in operand.flags
                )
            )
            if writes_sgpr and any(
                alternative.reg_class != _REG_SGPR for alternative in operand.reg_alts
            ):
                raise ValueError(
                    f"AMDGPU descriptor '{descriptor.key}' result "
                    f"'{operand.field_name}' requires distinct SGPR and VGPR forms "
                    "to model VALU write timing"
                )
            operands.append(
                replace(
                    operand,
                    read_event=read_event if reads_sgpr else operand.read_event,
                    write_event=write_event if writes_sgpr else operand.write_event,
                )
            )
        descriptors.append(replace(descriptor, operands=tuple(operands)))
    return replace(
        descriptor_set,
        descriptors=tuple(descriptors),
        timing_events=(
            *descriptor_set.timing_events,
            TimingEvent(read_event),
            TimingEvent(write_event),
        ),
        event_separations=(
            *descriptor_set.event_separations,
            EventSeparation(
                write_event, read_event, separation_cycles, ModelQuality.ESTIMATED
            ),
        ),
    )
