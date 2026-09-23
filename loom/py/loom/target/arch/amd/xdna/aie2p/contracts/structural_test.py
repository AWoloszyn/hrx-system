# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for AMD XDNA AIE2P structural vector contracts."""

from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.structural import (
    _I16_INTERLEAVE_CONTROL,
    _I32_F32_TRANSPOSE_4X4_CONTROL,
    _VECTOR_CARRIER_SPECS,
    _WIDE_VECTOR_EXTRACT_SPECS,
    AIE2P_STRUCTURAL_RULES,
)
from loom.target.contracts import (
    AttrProject,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    Guard,
    ValueAliasRule,
    Vector,
)


def _wide_extract_rule(
    source_type,
    result_type,
    *index_guards: Guard,
) -> DescriptorRule:
    return next(
        rule
        for rule in AIE2P_STRUCTURAL_RULES
        if isinstance(rule, DescriptorRule)
        and rule.source_op is vector.vector_extract
        and Guard.value_type("source", source_type) in rule.guards
        and Guard.value_type("result", result_type) in rule.guards
        and all(guard in rule.guards for guard in index_guards)
    )


def test_wide_static_extracts_select_and_normalize_each_half() -> None:
    for source_type, result_type, half_lanes, storage, _ in _WIDE_VECTOR_EXTRACT_SPECS:
        low_rule = _wide_extract_rule(
            source_type,
            result_type,
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_element_range("static_indices", 0, 0, half_lanes - 1),
        )
        high_rule = _wide_extract_rule(
            source_type,
            result_type,
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_element_range(
                "static_indices", 0, half_lanes, 2 * half_lanes - 1
            ),
        )

        assert low_rule.descriptor.key == (
            f"amd.xdna.aie2p.extract.{storage}.immediate"
        )
        assert isinstance(low_rule.emit[0], EmitRegisterSlice)
        assert low_rule.emit[0].unit_offset == 0
        assert low_rule.emit[0].unit_count == 2
        assert isinstance(low_rule.emit[1], EmitDescriptorOp)
        assert low_rule.emit[1].immediates == {
            "idx": AttrProject.i64_array_element("static_indices", element=0)
        }

        assert isinstance(high_rule.emit[0], EmitRegisterSlice)
        assert high_rule.emit[0].unit_offset == 2
        assert high_rule.emit[0].unit_count == 2
        assert isinstance(high_rule.emit[1], EmitDescriptorOp)
        assert high_rule.emit[1].immediates == {
            "idx": AttrProject.i64_array_element_plus_literal(
                "static_indices", element=0, literal=-half_lanes
            )
        }


def test_wide_dynamic_extract_selects_one_physical_half() -> None:
    source_type, result_type, half_lanes, storage, _ = next(
        spec for spec in _WIDE_VECTOR_EXTRACT_SPECS if spec[1].element == "i32"
    )
    rule = _wide_extract_rule(
        source_type,
        result_type,
        Guard.operand_segment_count("indices", 1),
    )

    assert [
        emit.descriptor.key for emit in rule.emit if isinstance(emit, EmitDescriptorOp)
    ] == [
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.and.i32",
        "amd.xdna.aie2p.and.i32",
        f"amd.xdna.aie2p.extract.{storage}.register",
        f"amd.xdna.aie2p.extract.{storage}.register",
        "amd.xdna.aie2p.select.nonzero.i32",
    ]
    assert rule.emit[2].immediates == {"i": half_lanes - 1}
    assert rule.emit[3].immediates == {"i": half_lanes}


def test_wide_pair_extract_selects_each_scalar_word() -> None:
    source_type, result_type, _, _, _ = next(
        spec for spec in _WIDE_VECTOR_EXTRACT_SPECS if spec[1].element == "i64"
    )
    rule = _wide_extract_rule(
        source_type,
        result_type,
        Guard.operand_segment_count("indices", 1),
    )

    assert isinstance(rule.emit[-1], EmitRegisterConcat)
    assert tuple(source.field for source in rule.emit[-1].sources) == (
        "selected_0",
        "selected_1",
    )


def _slice_rule(
    source_type,
    result_type,
    offset_minimum: int,
    offset_maximum: int,
):
    return next(
        rule
        for rule in AIE2P_STRUCTURAL_RULES
        if rule.source_op is vector.vector_slice
        and Guard.value_type("source", source_type) in rule.guards
        and Guard.value_type("result", result_type) in rule.guards
        and Guard.i64_array_element_range(
            "static_offsets", 0, offset_minimum, offset_maximum
        )
        in rule.guards
    )


def test_static_slices_project_logical_lanes_into_physical_carriers() -> None:
    slice_rules = [
        rule for rule in AIE2P_STRUCTURAL_RULES if rule.source_op is vector.vector_slice
    ]
    assert len(slice_rules) == 8 * len(_VECTOR_CARRIER_SPECS)

    for element_types, element_byte_count, wide_lane_maximum in _VECTOR_CARRIER_SPECS:
        carrier_lane_count = 64 // element_byte_count
        narrow_type = Vector(
            element_types,
            minimum_lanes=1,
            maximum_lanes=carrier_lane_count,
        )
        wide_type = Vector(
            element_types,
            minimum_lanes=carrier_lane_count + 1,
            maximum_lanes=wide_lane_maximum,
        )

        narrow_low = _slice_rule(narrow_type, narrow_type, 0, 0)
        assert isinstance(narrow_low, ValueAliasRule)
        narrow_shift = _slice_rule(
            narrow_type,
            narrow_type,
            1,
            carrier_lane_count - 1,
        )
        assert narrow_shift.emit[0].immediates == {
            "i": AttrProject.i64_array_lane_byte_offset(
                "static_offsets",
                element=0,
                bytes_per_lane=element_byte_count,
            )
        }

        wide_low = _slice_rule(wide_type, narrow_type, 0, 0)
        assert isinstance(wide_low.emit[0], EmitRegisterSlice)
        assert wide_low.emit[0].unit_offset == 0
        wide_crossing = _slice_rule(
            wide_type,
            narrow_type,
            1,
            carrier_lane_count - 1,
        )
        assert [emit.unit_offset for emit in wide_crossing.emit[:2]] == [0, 2]
        wide_high = _slice_rule(
            wide_type,
            narrow_type,
            carrier_lane_count,
            carrier_lane_count,
        )
        assert wide_high.emit[0].unit_offset == 2
        high_shift = _slice_rule(
            wide_type,
            narrow_type,
            carrier_lane_count + 1,
            wide_lane_maximum - 1,
        )
        assert high_shift.emit[1].immediates == {
            "i": AttrProject.i64_array_lane_byte_offset(
                "static_offsets",
                element=0,
                bytes_per_lane=element_byte_count,
                base_byte_offset=-64,
            )
        }

        wide_alias = _slice_rule(wide_type, wide_type, 0, 0)
        assert isinstance(wide_alias, ValueAliasRule)
        wide_shift = _slice_rule(
            wide_type,
            wide_type,
            1,
            carrier_lane_count - 1,
        )
        assert [type(emit) for emit in wide_shift.emit] == [
            EmitRegisterSlice,
            EmitRegisterSlice,
            EmitDescriptorOp,
            EmitDescriptorOp,
            EmitDescriptorOp,
            EmitRegisterConcat,
        ]
        assert wide_shift.emit[2].immediates == {
            "i": AttrProject.i64_array_lane_byte_offset(
                "static_offsets",
                element=0,
                bytes_per_lane=element_byte_count,
            )
        }


def test_i32_f32_4x4_transpose_uses_native_shuffle_mode() -> None:
    rule = next(
        rule
        for rule in AIE2P_STRUCTURAL_RULES
        if isinstance(rule, DescriptorRule)
        and rule.source_op is vector.vector_transpose
    )

    assert [
        emit.descriptor.key for emit in rule.emit if isinstance(emit, EmitDescriptorOp)
    ] == [
        "amd.xdna.aie2p.constant.i32.mova",
        "amd.xdna.aie2p.shuffle.x.configured",
    ]
    assert rule.emit[0].immediates == {"i": _I32_F32_TRANSPOSE_4X4_CONTROL}


def test_16bit_interleave_uses_alternating_native_shuffle() -> None:
    rule = next(
        rule
        for rule in AIE2P_STRUCTURAL_RULES
        if isinstance(rule, DescriptorRule)
        and rule.source_op is vector.vector_interleave
    )
    assert len(rule.emit) == 2
    assert rule.emit[0].immediates == {"i": _I16_INTERLEAVE_CONTROL}
    assert _I16_INTERLEAVE_CONTROL == 18
    assert rule.emit[1].descriptor.key == "amd.xdna.aie2p.shuffle.x.configured"
    assert rule.emit[1].operands["s1"].field == "even"
    assert rule.emit[1].operands["s2"].field == "odd"
    assert Guard.i64_range("axis", 0, 0) in rule.guards
