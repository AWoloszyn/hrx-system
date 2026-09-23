# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# ruff: noqa: F403, F405

"""Full-width vector integer multiply operands and constant forms."""

from __future__ import annotations

from .common import *

_RESULT_CONSTRAINTS = (Constraint(ConstraintKind.REMATERIALIZABLE, 0),)


def _v_mul_integer_overlay(
    product: str, scalar: str, *, include_literal_forms: bool
) -> AmdgpuDescriptorOverlay:
    mnemonic = f"v_mul_{product}_{scalar}"
    modes = ("inline", "lit") if include_literal_forms else ("inline",)
    return AmdgpuDescriptorOverlay(
        descriptor_key=f"amdgpu.{mnemonic}",
        instruction_name=mnemonic.upper(),
        mnemonic=mnemonic,
        encoding_name="ENC_VOP3",
        semantic_tag=f"integer.mul.{product}.{scalar}",
        schedule_class=_SCHEDULE_VALU,
        operands=(
            AmdgpuOperandOverlay("VDST", _vgpr_result()),
            AmdgpuOperandOverlay("SRC0", _sgpr_vgpr_operand("lhs")),
            AmdgpuOperandOverlay("SRC1", _vgpr_operand("rhs")),
        ),
        operand_forms=tuple(
            _literal_operand_form(
                replacement_descriptor=f"amdgpu.{mnemonic}.{source}_{mode}",
                source_operand=operand,
            )
            for mode in modes
            for source, operand in (("src0", "lhs"), ("src1", "rhs"))
        ),
        constraints=_RESULT_CONSTRAINTS,
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


def _v_mul_integer_constant_overlay(
    product: str, scalar: str, source: str, mode: str
) -> AmdgpuDescriptorOverlay:
    mnemonic = f"v_mul_{product}_{scalar}"
    source_fields = {
        "src0": ("SRC0", "lhs", _sgpr_vgpr_operand("lhs")),
        "src1": ("SRC1", "rhs", _vgpr_operand("rhs")),
    }
    constant_field = source_fields[source][0]
    remaining_operands = tuple(
        fields for name, fields in source_fields.items() if name != source
    )
    encoding_fields = (
        dict(
            immediate_fields=(constant_field,),
            immediates=(_SOURCE_INLINE_U32_IMMEDIATE,),
        )
        if mode == "inline"
        else dict(
            encoding_format_id=AMDGPU_ENCODING_FORMAT_VOP3_LITERAL,
            immediates=(_LITERAL_U32_IMMEDIATE,),
            fixed_encoding_fields=(
                (constant_field, _predefined("SRC_LITERAL", "OPR_SRC")),
            ),
        )
    )
    return AmdgpuDescriptorOverlay(
        descriptor_key=f"amdgpu.{mnemonic}.{source}_{mode}",
        instruction_name=mnemonic.upper(),
        mnemonic=mnemonic,
        encoding_name="ENC_VOP3",
        semantic_tag=f"integer.mul.{product}.{scalar}",
        schedule_class=_SCHEDULE_VALU,
        operands=(
            AmdgpuOperandOverlay("VDST", _vgpr_result()),
            *(
                AmdgpuOperandOverlay(field, operand)
                for field, _, operand in remaining_operands
            ),
        ),
        asm_forms=_asm(
            mnemonic=f"{mnemonic}_{source}_{mode}",
            results=("dst",),
            operands=tuple(name for _, name, _ in remaining_operands),
            immediates=("imm32",),
        ),
        constraints=_RESULT_CONSTRAINTS,
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
        **encoding_fields,
    )


def _v_mul_integer_overlays(
    *, include_literal_forms: bool = True
) -> tuple[AmdgpuDescriptorOverlay, ...]:
    modes = ("inline", "lit") if include_literal_forms else ("inline",)
    return tuple(
        overlay
        for product, scalar in (("lo", "u32"), ("hi", "u32"), ("hi", "i32"))
        for overlay in (
            _v_mul_integer_overlay(
                product, scalar, include_literal_forms=include_literal_forms
            ),
            *(
                _v_mul_integer_constant_overlay(product, scalar, source, mode)
                for mode in modes
                for source in ("src0", "src1")
            ),
        )
    )


__all__ = ["_v_mul_integer_overlays"]
