# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Completeness of C enum mappings."""

from loom.dsl import TypeConstraint
from loom.gen.ops.c_enums import TYPE_CONSTRAINT_MAP


def test_type_constraint_map_covers_every_constraint() -> None:
    assert set(TYPE_CONSTRAINT_MAP) == set(TypeConstraint)
