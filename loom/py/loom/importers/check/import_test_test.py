# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import contextlib
import io
import json
import sys
from pathlib import Path
from unittest import mock

from loom.importers.check.import_test import (
    _remove_script_directory_from_sys_path,
    main,
)
from loom.importers.check.registry import BackendRegistry, DeferredPackageBackend


def test_entrypoint_preserves_separate_paths_with_spaces_and_quotes(
    tmp_path: Path,
) -> None:
    registry = BackendRegistry()
    registry.register(
        DeferredPackageBackend(
            name="fixture", help="fixture importer", package="sys", extras=("fixture",)
        )
    )
    paths = [str(tmp_path / "first case.py"), str(tmp_path / "second case's input.py")]
    stdout = io.StringIO()
    with (
        mock.patch(
            "loom.importers.check.main.make_default_registry", return_value=registry
        ),
        contextlib.redirect_stdout(stdout),
    ):
        exit_code = main(["fixture", *paths, "--json"])

    assert exit_code == 0
    results = json.loads(stdout.getvalue())["results"]
    assert [result["path"] for result in results] == paths
    assert all(result["status"] == "skipped" for result in results)


def test_script_directory_is_removed_from_sys_path(
    tmp_path: Path,
) -> None:
    script_directory = tmp_path / "check"
    script_directory.mkdir()
    other_directory = tmp_path / "other"
    other_directory.mkdir()
    original_path = list(sys.path)
    try:
        sys.path[:] = [str(script_directory), str(other_directory)]

        _remove_script_directory_from_sys_path(script_directory)

        assert sys.path == [str(other_directory)]
    finally:
        sys.path[:] = original_path
