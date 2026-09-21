# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Descriptor invariants consumed by the native Wasm immediate readers."""

from loom.target.arch.wasm.descriptors import WASM_CORE_SIMD128_DESCRIPTOR_SET
from loom.target.low_descriptors import ImmediateFlag, ImmediateKind


def test_single_immediates_are_numeric_with_only_a_zero_offset_default():
    for descriptor in WASM_CORE_SIMD128_DESCRIPTOR_SET.descriptors:
        if descriptor.key in ("wasm.v128.const", "wasm.i8x16.shuffle"):
            continue
        assert len(descriptor.immediates) <= 1
        for immediate in descriptor.immediates:
            if descriptor.key in ("wasm.br", "wasm.br_if.i32"):
                # Native control flow uses structural Low operations; symbolic
                # branch packets are rejected before immediate emission.
                assert immediate.kind is ImmediateKind.ORDINAL
                assert immediate.flags == (ImmediateFlag.SYMBOLIC,)
                continue
            assert immediate.kind in (ImmediateKind.SIGNED, ImmediateKind.UNSIGNED)
            if ImmediateFlag.DEFAULT_VALUE in immediate.flags:
                assert immediate.field_name == "offset"
                assert immediate.kind is ImmediateKind.UNSIGNED
                assert immediate.bit_width == 32
                assert immediate.unsigned_max == (1 << 32) - 1
                assert immediate.default_value == 0
            else:
                assert not immediate.flags


def test_vector_constant_words_are_required_full_width_bits():
    descriptor = next(
        descriptor
        for descriptor in WASM_CORE_SIMD128_DESCRIPTOR_SET.descriptors
        if descriptor.key == "wasm.v128.const"
    )
    assert sorted(value.field_name for value in descriptor.immediates) == [
        "hi64",
        "lo64",
    ]
    assert [value.field_name for value in descriptor.asm_forms[0].immediates] == [
        "lo64",
        "hi64",
    ]
    for immediate in descriptor.immediates:
        assert immediate.kind is ImmediateKind.UNSIGNED
        assert immediate.bit_width == 64
        assert immediate.unsigned_max == (1 << 64) - 1
        assert not immediate.flags


def test_shuffle_has_sixteen_required_lanes_from_both_inputs():
    descriptor = next(
        descriptor
        for descriptor in WASM_CORE_SIMD128_DESCRIPTOR_SET.descriptors
        if descriptor.key == "wasm.i8x16.shuffle"
    )
    wire_names = [f"lane{lane}" for lane in range(16)]
    assert sorted(value.field_name for value in descriptor.immediates) == sorted(
        wire_names
    )
    assert [
        value.field_name for value in descriptor.asm_forms[0].immediates
    ] == wire_names
    for immediate in descriptor.immediates:
        assert immediate.kind is ImmediateKind.UNSIGNED
        assert immediate.unsigned_max == 31
        assert not immediate.flags
