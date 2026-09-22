# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Artifact ownership policy for checked-in and build-generated files."""

from loom.dsl import Dialect
from loom.gen.ops import model as c_table_model
from loom.gen.ops.c_tables import checked_in_file_set


def test_checked_in_file_set_separates_public_artifacts_from_build_outputs() -> None:
    dialect = Dialect("artifact_test", dialect_id=0x7D)
    build_generated_dialect = Dialect(
        "build_generated_test",
        dialect_id=0x7C,
        checked_in_headers=False,
    )
    model = c_table_model.GenerationModel(
        dialects=[
            c_table_model.DialectGeneration(
                dialect=dialect,
                ops=[],
                table_shards=None,
            ),
            c_table_model.DialectGeneration(
                dialect=build_generated_dialect,
                ops=[],
                table_shards=None,
            ),
        ],
        types=[],
    )

    generated_file_set = checked_in_file_set(model)

    assert "loom/src/loom/ops/artifact_test/ops.h" in generated_file_set.output_paths
    assert "loom/src/loom/ops/build_generated_test/ops.h" in generated_file_set.obsolete_paths
    assert "loom/src/loom/ops/op_registry.h" in generated_file_set.output_paths
    assert "loom/src/loom/ir/scalar_type_table.inc" in generated_file_set.output_paths
    assert "loom/src/loom/ops/artifact_test/ops.inc" in generated_file_set.obsolete_paths
    assert "loom/src/loom/ops/build_generated_test/ops.inc" in generated_file_set.obsolete_paths
    assert "loom/src/loom/ops/artifact_test/builders.c" in generated_file_set.obsolete_paths
    assert "loom/src/loom/ops/artifact_test/tables.c" in generated_file_set.obsolete_paths
    assert "loom/src/loom/ops/op_registry_tables.c" in generated_file_set.obsolete_paths
    assert "loom/src/loom/ops/op_registry_tables.h" in generated_file_set.obsolete_paths
    assert all(not path.endswith(".c") for path in generated_file_set.output_paths)
    assert set(generated_file_set.output_paths).isdisjoint(generated_file_set.obsolete_paths)
