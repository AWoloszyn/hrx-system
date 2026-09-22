# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Collision rejection when declarations project into C names."""

import pytest

from loom.dsl import Dialect, EncodingFamilyDef, EncodingFamilyRole
from loom.gen.ops.c_metadata_tables import generate_tables_c
from loom.gen.ops.c_ops_header import generate_ops_h


def test_rejects_colliding_encoding_family_c_names() -> None:
    dialect = Dialect("encoding", dialect_id=0x09)
    families = [
        EncodingFamilyDef(
            name,
            group=dialect,
            role=EncodingFamilyRole.STORAGE_SCHEMA,
        )
        for name in ("ggml.q4_k", "ggml_q4_k")
    ]

    with pytest.raises(ValueError, match=r"encoding families 'ggml\.q4_k' and 'ggml_q4_k' both generate C prefix"):
        generate_ops_h("encoding", 0x09, [], (), families)
    with pytest.raises(ValueError, match=r"encoding families 'ggml\.q4_k' and 'ggml_q4_k' both generate C prefix"):
        generate_tables_c("encoding", 0x09, [], (), families)


def test_rejects_duplicate_encoding_family_names() -> None:
    family = EncodingFamilyDef(
        "ggml.q4_k",
        group=Dialect("encoding", dialect_id=0x09),
        role=EncodingFamilyRole.STORAGE_SCHEMA,
    )

    with pytest.raises(ValueError, match=r"duplicate encoding family 'ggml\.q4_k'"):
        generate_ops_h("encoding", 0x09, [], (), [family, family])
