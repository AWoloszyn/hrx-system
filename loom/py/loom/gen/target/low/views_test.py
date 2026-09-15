# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Register vocabulary and allocation contracts in shared descriptor views."""

from __future__ import annotations

from dataclasses import replace

import pytest

from loom.gen.target.low import compiler, views
from loom.gen.target.low.low_descriptors import generate_descriptor_set_family
from loom.target.low_descriptors import DescriptorSet
from loom.target.test.descriptors import (
    TEST_LOW_ADD_I32_DESCRIPTOR,
    TEST_LOW_CORE_DESCRIPTOR_SET,
)


def _view_spec() -> DescriptorSet:
    storage = TEST_LOW_CORE_DESCRIPTOR_SET
    classes = {item.name: item for item in storage.reg_classes}
    return replace(
        storage,
        key="test.low.register_view.core",
        function_name="loom_test_low_register_view_core_descriptor_set",
        c_table_prefix="TestLowRegisterViewCore",
        c_enum_prefix="TEST_LOW_REGISTER_VIEW_CORE",
        reg_classes=(
            classes["test.i32"],
            replace(
                classes["test.phys"],
                allocatable_count=16,
                fixed_location_base=16,
                fixed_location_count=4,
            ),
        ),
        descriptors=(TEST_LOW_ADD_I32_DESCRIPTOR,),
    )


def test_view_preserves_capacity_fixed_locations_and_absence() -> None:
    compiled = compiler.compile_descriptor_set(TEST_LOW_CORE_DESCRIPTOR_SET)
    spec = _view_spec()
    view = views.descriptor_set_view_for_spec(compiled, spec)
    assert len(view.reg_classes) == len(compiled.reg_classes)
    assert view.reg_classes[compiled.reg_class_ids["test.phys"]] == spec.reg_classes[1]
    assert view.reg_classes[compiled.reg_class_ids["test.f32"]] is None
    assert compiled.reg_classes[compiled.reg_class_ids["test.phys"]].allocatable_count == 32


def test_equal_view_register_tables_share_storage() -> None:
    spec = _view_spec()
    alias = replace(
        spec,
        key="test.low.register_alias.core",
        function_name="loom_test_low_register_alias_core_descriptor_set",
        c_table_prefix="TestLowRegisterAliasCore",
    )
    source = generate_descriptor_set_family(TEST_LOW_CORE_DESCRIPTOR_SET, (spec, alias)).source
    assert source.count(".reg_classes = kTestLowRegisterViewCoreRegClasses,") == 2
    assert "kTestLowRegisterAliasCoreRegClasses[]" not in source
    assert "kTestLowCoreRegClasses[]" not in source
    assert ".name_string_offset = LOOM_LOW_STRING_OFFSET_NONE," in source
    assert source.count(".operands = kTestLowCoreOperands,") == 2


@pytest.mark.parametrize(
    "changes",
    [
        {"alloc_unit_bits": 256},
        {"target_bank_id": 1},
        {"full_register_part_mask": 3},
    ],
)
def test_view_rejects_storage_identity_changes(changes: dict[str, int]) -> None:
    compiled = compiler.compile_descriptor_set(TEST_LOW_CORE_DESCRIPTOR_SET)
    spec = _view_spec()
    spec = replace(
        spec,
        reg_classes=(spec.reg_classes[0], replace(spec.reg_classes[1], **changes)),
    )
    with pytest.raises(ValueError, match="differs from storage outside allocation"):
        views.descriptor_set_view_for_spec(compiled, spec)


def test_view_rejects_descriptor_reference_to_absent_class() -> None:
    compiled = compiler.compile_descriptor_set(TEST_LOW_CORE_DESCRIPTOR_SET)
    spec = _view_spec()
    spec = replace(spec, reg_classes=spec.reg_classes[1:])
    with pytest.raises(ValueError, match=r"references absent register classes: test\.i32"):
        views.descriptor_set_view_for_spec(compiled, spec)


def test_view_rejects_invalid_fixed_location_geometry() -> None:
    compiled = compiler.compile_descriptor_set(TEST_LOW_CORE_DESCRIPTOR_SET)
    spec = _view_spec()
    spec = replace(
        spec,
        reg_classes=(
            spec.reg_classes[0],
            replace(spec.reg_classes[1], fixed_location_base=15),
        ),
    )
    with pytest.raises(ValueError, match="fixed-location range overlaps"):
        views.descriptor_set_view_for_spec(compiled, spec)


def test_view_retains_alias_indices_from_shared_namespace() -> None:
    compiled = compiler.compile_descriptor_set(TEST_LOW_CORE_DESCRIPTOR_SET)
    classes = {item.name: item for item in compiled.reg_classes}
    spec = replace(
        _view_spec(),
        reg_classes=(classes["test.i32"], classes["test.pressure.alias32"]),
    )
    view = views.descriptor_set_view_for_spec(compiled, spec)
    row = view.reg_classes[compiled.reg_class_ids["test.pressure.alias32"]]
    assert row is not None and row.alias_set_id == 2
    assert view.reg_classes[compiled.reg_class_ids["test.alias32"]] is None
