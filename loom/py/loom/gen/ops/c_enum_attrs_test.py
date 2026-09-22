# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared enum-domain contracts for encoding families."""

import pytest

from loom.dsl import Dialect, EncodingFamilyDef, EncodingFamilyRole, EnumCase, EnumDef
from loom.gen.ops.c_ops_header import generate_ops_h


def test_encoding_families_require_one_auxiliary_key_vocabulary() -> None:
    dialect = Dialect("encoding", dialect_id=0x09)
    first_keys = EnumDef("FirstKeys", [EnumCase("scale", 0)])
    second_keys = EnumDef("SecondKeys", [EnumCase("minimum", 0)])
    families = [
        EncodingFamilyDef(
            "first",
            group=dialect,
            role=EncodingFamilyRole.STORAGE_SCHEMA,
            auxiliary_key_enum=first_keys,
        ),
        EncodingFamilyDef(
            "second",
            group=dialect,
            role=EncodingFamilyRole.STORAGE_SCHEMA,
            auxiliary_key_enum=second_keys,
        ),
    ]

    with pytest.raises(ValueError, match="encoding families in one dialect must share one auxiliary key enum"):
        generate_ops_h("encoding", 0x09, [], (), families)
