# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

from loom.assembly import Flags
from loom.dsl import AttrDef, Dialect, EnumCase, EnumDef, Op
from loom.gen.ops.c_metadata_tables import _emit_instance_flags, generate_tables_c


def _flagged_op(dialect: Dialect, name: str, flags: EnumDef) -> Op:
    return Op(
        name,
        group=dialect,
        attrs=[AttrDef("flags", "flags", optional=True, enum_def=flags)],
        format=[Flags("flags")],
    )


def test_sparse_bits_and_aliases_preserve_the_valid_mask() -> None:
    flags = EnumDef(
        "Flags",
        [EnumCase("all", 9), EnumCase("high", 8), EnumCase("none", 0), EnumCase("low", 1)],
    )
    assert _emit_instance_flags([], "test_flags", flags) == 9


def test_all_eight_flag_bits_fit_with_an_aggregate_alias() -> None:
    flags = EnumDef("Flags", [*(EnumCase(f"bit{i}", 1 << i) for i in range(8)), EnumCase("all", 255)])
    assert _emit_instance_flags([], "test_flags", flags) == 255


def test_alias_requires_individually_printable_bits() -> None:
    flags = EnumDef("Flags", [EnumCase("low", 1), EnumCase("all", 3)])
    dialect = Dialect("test")
    with pytest.raises(ValueError, match="alias 'all' contains bits without individual spellings"):
        generate_tables_c("test", 0, [_flagged_op(dialect, "test.flags", flags)])
