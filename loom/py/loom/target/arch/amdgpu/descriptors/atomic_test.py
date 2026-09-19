# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.target.arch.amdgpu.descriptors.sets import (
    _gfx9_4_generic_core_overlays,
    _gfx940_core_overlays,
    _gfx950_core_overlays,
)
from loom.target.low_descriptors import MemorySpace, OperandRole


def test_cdna_global_integer_atomics_preserve_return_and_native_spelling() -> None:
    for overlays in (
        _gfx940_core_overlays(),
        _gfx950_core_overlays(),
        _gfx9_4_generic_core_overlays(),
    ):
        descriptors = {row.descriptor_key: row for row in overlays}
        for descriptor_suffix, mnemonic_suffix, semantic_suffix in (
            ("add_u32", "add", "add.u32"),
            ("sub_u32", "sub", "sub.u32"),
            ("min_i32", "smin", "min.i32"),
            ("max_i32", "smax", "max.i32"),
            ("min_u32", "umin", "min.u32"),
            ("max_u32", "umax", "max.u32"),
            ("and_b32", "and", "and.b32"),
            ("or_b32", "or", "or.b32"),
            ("xor_b32", "xor", "xor.b32"),
            ("swap_b32", "swap", "exchange.b32"),
        ):
            return_forms = (True,) if mnemonic_suffix == "swap" else (False, True)
            for returns_old_value in return_forms:
                return_suffix = "_rtn" if returns_old_value else ""
                descriptor = descriptors[
                    f"amdgpu.global_atomic_{descriptor_suffix}{return_suffix}_saddr"
                ]
                assert descriptor.instruction_name == (
                    f"GLOBAL_ATOMIC_{mnemonic_suffix.upper()}"
                )
                assert descriptor.mnemonic == f"global_atomic_{mnemonic_suffix}"
                assert descriptor.semantic_tag == (
                    f"memory.global.atomic.{semantic_suffix}"
                    + (".return" if returns_old_value else "")
                )
                assert dict(descriptor.fixed_encoding_fields)["SC0"] == int(
                    returns_old_value
                )
                results = tuple(
                    operand.descriptor_operand
                    for operand in descriptor.operands
                    if operand.descriptor_operand.role is OperandRole.RESULT
                )
                assert len(results) == int(returns_old_value)
                assert all(result.unit_count == 1 for result in results)
                assert tuple(effect.memory_space for effect in descriptor.effects) == (
                    MemorySpace.GLOBAL,
                    MemorySpace.GLOBAL,
                )
                assert tuple(effect.width_bits for effect in descriptor.effects) == (
                    32,
                    32,
                )
                assert descriptor.asm_forms[0].mnemonic == (
                    f"global_atomic_{mnemonic_suffix}{return_suffix}_saddr"
                )
