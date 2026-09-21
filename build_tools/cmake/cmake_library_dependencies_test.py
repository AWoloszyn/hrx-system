# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import configured_cmake_arguments

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/library_dependencies"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CONFIGURATION = os.environ.get("IREE_TEST_CMAKE_BUILD_TYPE") or "Release"


class CMakeLibraryDependenciesTest(unittest.TestCase):
    def run_command(self, *arguments: str) -> str:
        result = subprocess.run(
            arguments,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def test_pruning_preserves_generated_inputs_and_final_link_dependencies(self):
        for optimized in ("OFF", "ON"):
            with (
                self.subTest(optimized=optimized),
                tempfile.TemporaryDirectory(
                    prefix="cmake library fixture "
                ) as temporary_dir,
            ):
                root = Path(temporary_dir)
                source_dir = root / "source with spaces"
                build_dir = root / "build with spaces"
                shutil.copytree(FIXTURE_SOURCE_DIR, source_dir)
                self.run_command(
                    CMAKE_COMMAND,
                    "-S",
                    str(source_dir),
                    "-B",
                    str(build_dir),
                    *configured_cmake_arguments(),
                    f"-DIREE_REPO_ROOT={REPO_ROOT}",
                    f"-DCMAKE_OPTIMIZE_DEPENDENCIES={optimized}",
                )
                leaf, *programs = [
                    Path(path)
                    for path in (build_dir / f"paths-{CONFIGURATION}.txt")
                    .read_text()
                    .splitlines()
                ]
                build_command = (
                    CMAKE_COMMAND,
                    "--build",
                    str(build_dir),
                    "--config",
                    CONFIGURATION,
                    "--parallel",
                    "4",
                    "--target",
                )
                self.run_command(*build_command, "iree_fixture_library.objects")
                generated_header = build_dir / "generated.h"
                self.assertTrue(generated_header.is_file())
                if optimized == "ON":
                    self.assertFalse(leaf.exists(), "Compilation needs no archive")

                def build_and_run(expected: int):
                    self.run_command(*build_command, "probe", "unified_probe")
                    for program in programs:
                        self.assertEqual(
                            self.run_command(str(program)), f"{expected}\n"
                        )

                build_and_run(15)
                modification_times = [path.stat().st_mtime_ns for path in programs]
                build_and_run(15)
                self.assertEqual(
                    [path.stat().st_mtime_ns for path in programs], modification_times
                )

                (source_dir / "value.txt").write_text("11\n")
                build_and_run(19)
                leaf_source = source_dir / "leaf.c"
                leaf_source.write_text(
                    leaf_source.read_text().replace("return 5;", "return 6;")
                )
                build_and_run(20)
                generated_header.unlink()
                build_and_run(20)


if __name__ == "__main__":
    unittest.main()
