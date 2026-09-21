# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import configured_cmake_arguments

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/python_dependencies"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]


class CMakePythonDependenciesTest(unittest.TestCase):
    def run_command(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            arguments,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def configure(self, source_dir: Path, build_dir: Path, *arguments: str):
        return self.run_command(
            CMAKE_COMMAND,
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            *configured_cmake_arguments(),
            f"-DIREE_REPO_ROOT={REPO_ROOT}",
            f"-DPython3_EXECUTABLE={sys.executable}",
            *arguments,
        )

    def build_outputs(self, build_dir: Path, expected: str):
        result = self.run_command(
            CMAKE_COMMAND,
            "--build",
            str(build_dir),
            "--config",
            "Release",
            "--target",
            "iree_generated_compile_inputs",
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        for index in range(3):
            self.assertEqual((build_dir / f"output_{index}.txt").read_text(), expected)

    def test_shared_sources_remain_build_inputs_and_import_order_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            source_dir = root / "source"
            shutil.copytree(FIXTURE_SOURCE_DIR, source_dir)
            build_dir = root / "build"
            result = self.configure(source_dir, build_dir)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(
                [
                    Path(path)
                    for path in (build_dir / "sources.txt").read_text().split(";")
                ],
                [
                    source_dir / path
                    for path in (
                        "generator.py",
                        "left/choice.py",
                        "right/choice.py",
                        "shared/value.py",
                    )
                ],
            )
            self.build_outputs(build_dir, "left: initial\n")

            outputs = [build_dir / f"output_{index}.txt" for index in range(3)]
            modification_times = [path.stat().st_mtime_ns for path in outputs]
            self.build_outputs(build_dir, "left: initial\n")
            self.assertEqual(
                [path.stat().st_mtime_ns for path in outputs], modification_times
            )

            value = source_dir / "shared/value.py"
            value.write_text(value.read_text().replace('"initial"', '"changed"'))
            self.build_outputs(build_dir, "left: changed\n")

            result = self.configure(
                source_dir, build_dir, "-DIREE_TEST_REVERSE_DEPS=ON"
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.build_outputs(build_dir, "right: changed\n")

    def test_shared_graph_work_scales_with_declarations(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            command_counts = []
            for depth in (4, 8):
                trace = root / f"trace-{depth}.jsonl"
                build_dir = root / f"build-{depth}"
                result = self.configure(
                    FIXTURE_SOURCE_DIR,
                    build_dir,
                    f"-DIREE_TEST_DIAMOND_DEPTH={depth}",
                    "--trace-format=json-v1",
                    f"--trace-source={REPO_ROOT}/build_tools/cmake/iree_python.cmake",
                    f"--trace-redirect={trace}",
                )
                self.assertEqual(result.returncode, 0, result.stdout)
                self.build_outputs(build_dir, "left: initial\n")
                with trace.open() as stream:
                    command_counts.append(sum(1 for _ in stream))
            # Doubling depth must not approach the 16x growth in paths.
            # Count interpreted commands instead of asserting host-dependent time.
            self.assertLess(command_counts[1], 3 * command_counts[0], command_counts)

    def test_rejects_missing_transitive_library(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            result = self.configure(
                FIXTURE_SOURCE_DIR,
                Path(temporary_dir) / "build",
                "-DIREE_TEST_MISSING_DEP=ON",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "iree_py_library target fixture::missing was not found",
                " ".join(result.stdout.split()),
            )


if __name__ == "__main__":
    unittest.main()
