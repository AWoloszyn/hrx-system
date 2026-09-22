# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Selection of dialect declaration loaders."""

from loom.gen.ops import model as c_table_model


def test_load_dialect_generation_calls_only_requested_loader() -> None:
    calls: list[str] = []
    expected = c_table_model.DialectGeneration(dialect=object(), ops=[], table_shards=None)
    other = c_table_model.DialectGeneration(dialect=object(), ops=[], table_shards=None)

    def load_other() -> c_table_model.DialectGeneration:
        calls.append("other")
        return other

    def load_wanted() -> c_table_model.DialectGeneration:
        calls.append("wanted")
        return expected

    original_loaders = c_table_model._DIALECT_GENERATION_LOADERS
    try:
        c_table_model._DIALECT_GENERATION_LOADERS = (
            ("other", load_other),
            ("wanted", load_wanted),
        )
        actual = c_table_model.load_dialect_generation("wanted")
    finally:
        c_table_model._DIALECT_GENERATION_LOADERS = original_loaders

    assert actual is expected
    assert calls == ["wanted"]
