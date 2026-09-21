# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from build_tools.cmake.test_environment import configured_cmake_arguments
from build_tools.devtools import ctest as ctest_dev
from build_tools.devtools.environment import REPO_ROOT

FIXTURE_SOURCE_DIR = REPO_ROOT / "build_tools/cmake/testdata/test_metadata"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CTEST_COMMAND = os.environ["IREE_TEST_CTEST_COMMAND"]


class CTestIntegrationTest(unittest.TestCase):
    def test_shared_closure_is_fresh_and_visited_once(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            source_dir = root / "source with spaces"
            build_dir = root / "build with spaces"
            shutil.copytree(FIXTURE_SOURCE_DIR.parent / "selected_build", source_dir)
            subprocess.run(
                [
                    CMAKE_COMMAND,
                    "-S",
                    str(source_dir),
                    "-B",
                    str(build_dir),
                    *configured_cmake_arguments(),
                    f"-DIREE_REPO_ROOT={REPO_ROOT}",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            step = ctest_dev.CTestBuildAndRunStep(
                cmake=CMAKE_COMMAND,
                ctest=CTEST_COMMAND,
                build_dir=build_dir,
                arguments=["-R", "^(left|right)$", "-C", "Release"],
                cwd=REPO_ROOT,
                env={**os.environ, "CMAKE_BUILD_PARALLEL_LEVEL": "2"},
            )
            make_temporary_dir = root / "make $files = # '"
            make_temporary_dir.mkdir()

            def check_build(build_count):
                # Exercise the real Make/shell argument boundary with a
                # temporary-file location containing metacharacters.
                with mock.patch.object(tempfile, "tempdir", str(make_temporary_dir)):
                    self.assertEqual(step.run(), 0)
                self.assertEqual(list(make_temporary_dir.iterdir()), [])
                generator = os.environ["IREE_TEST_CMAKE_GENERATOR"]
                if generator == "Unix Makefiles" or generator.startswith("Ninja"):
                    self.assertEqual(
                        (build_dir / "visits.txt").read_text().splitlines(),
                        ["visit"] * build_count,
                    )
                for name in ("left", "right"):
                    self.assertEqual(
                        (build_dir / f"{name}.ran").read_text().splitlines(),
                        ["run"] * build_count,
                    )

            check_build(1)
            result = subprocess.run(
                [
                    CTEST_COMMAND,
                    "--test-dir",
                    str(build_dir),
                    "-C",
                    "Release",
                    "-R",
                    "^left$",
                    "--show-only=json-v1",
                ],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            )
            executable = Path(json.loads(result.stdout)["tests"][0]["command"][0])
            initial_mtime = executable.stat().st_mtime_ns
            check_build(2)
            self.assertEqual(executable.stat().st_mtime_ns, initial_mtime)

            # The test executable reads the expected value at runtime, so stale
            # generated headers or compiled sources produce a real test failure.
            (source_dir / "value.txt").write_text("5\n")
            (source_dir / "expected.txt").write_text("6\n")
            check_build(3)
            source = source_dir / "shared.c"
            source.write_text(source.read_text().replace("+ 1", "+ 2"))
            (source_dir / "expected.txt").write_text("7\n")
            check_build(4)

            executable.unlink()
            check_build(5)
            self.assertTrue(executable.is_file())

            # Previously built executables still exist, but failed preparation
            # must stop before either test can execute them.
            source.write_text("#error selected build must stop\n")
            with mock.patch.object(tempfile, "tempdir", str(make_temporary_dir)):
                self.assertNotEqual(step.run(), 0)
            self.assertEqual(list(make_temporary_dir.iterdir()), [])
            for name in ("left", "right"):
                self.assertEqual(
                    (build_dir / f"{name}.ran").read_text().splitlines(), ["run"] * 5
                )

    def test_stale_graph_is_refreshed_before_ctest_selection(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            temporary_path = Path(temporary_dir)
            build_dir = temporary_path / "build"
            extension_file = temporary_path / "extension.cmake"
            extension_file.write_text("")
            subprocess.run(
                [
                    CMAKE_COMMAND,
                    "-S",
                    str(FIXTURE_SOURCE_DIR),
                    "-B",
                    str(build_dir),
                    *configured_cmake_arguments(),
                    f"-DIREE_REPO_ROOT={REPO_ROOT}",
                    f"-DIREE_TEST_METADATA_EXTENSION_FILE={extension_file}",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            extension_file.write_text("add_metadata_test(host-late host_late_root)\n")

            step = ctest_dev.CTestBuildAndRunStep(
                cmake=CMAKE_COMMAND,
                ctest=CTEST_COMMAND,
                build_dir=build_dir,
                arguments=["-R", "^host"],
                cwd=REPO_ROOT,
            )

            self.assertEqual(step.run(), 0)
            self.assertTrue((build_dir / "host.built").is_file())
            self.assertTrue((build_dir / "host-late.built").is_file())

    def test_selected_runner_builds_only_the_selected_closure(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            build_dir = Path(temporary_dir) / "build"
            subprocess.run(
                [
                    CMAKE_COMMAND,
                    "-S",
                    str(FIXTURE_SOURCE_DIR),
                    "-B",
                    str(build_dir),
                    *configured_cmake_arguments(),
                    f"-DIREE_REPO_ROOT={REPO_ROOT}",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            step = ctest_dev.CTestBuildAndRunStep(
                cmake=CMAKE_COMMAND,
                ctest=CTEST_COMMAND,
                build_dir=build_dir,
                arguments=["-R", "^source-only$"],
                cwd=REPO_ROOT,
            )
            self.assertEqual(step.run(), 0)
            self.assertFalse((build_dir / "host.built").exists())

            step = ctest_dev.CTestBuildAndRunStep(
                cmake=CMAKE_COMMAND,
                ctest=CTEST_COMMAND,
                build_dir=build_dir,
                arguments=["-R", "^host$"],
                cwd=REPO_ROOT,
            )
            self.assertEqual(step.run(), 0)
            self.assertTrue((build_dir / "host.built").is_file())
            self.assertFalse((build_dir / "benchmark.built").exists())

            step = ctest_dev.CTestBuildAndRunStep(
                cmake=CMAKE_COMMAND,
                ctest=CTEST_COMMAND,
                build_dir=build_dir,
                arguments=["-R", "^fixture-required$"],
                cwd=REPO_ROOT,
            )
            self.assertEqual(step.run(), 0)
            for test_name in (
                "fixture-setup",
                "fixture-required",
                "fixture-cleanup",
            ):
                self.assertTrue((build_dir / f"{test_name}.built").is_file())

            step = ctest_dev.CTestBuildAndRunStep(
                cmake=CMAKE_COMMAND,
                ctest=CTEST_COMMAND,
                build_dir=build_dir,
                arguments=["-R", "^tool-backed$"],
                cwd=REPO_ROOT,
            )
            self.assertEqual(step.run(), 0)
            self.assertTrue((build_dir / "tool.built").is_file())


if __name__ == "__main__":
    unittest.main()
