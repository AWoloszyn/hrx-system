# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import configured_cmake_arguments

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/installed_tests"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CTEST_COMMAND = os.environ["IREE_TEST_CTEST_COMMAND"]


class CMakeInstalledTestsTest(unittest.TestCase):
    def run_command(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            arguments,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def configure(self, build_dir: Path, *arguments: str):
        return self.run_command(
            CMAKE_COMMAND,
            "-S",
            str(FIXTURE_SOURCE_DIR),
            "-B",
            str(build_dir),
            *configured_cmake_arguments(),
            f"-DIREE_REPO_ROOT={REPO_ROOT}",
            *arguments,
        )

    def test_installs_shared_artifacts_at_every_referenced_destination(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            build_dir = root / "build"
            result = self.configure(build_dir)
            self.assertEqual(result.returncode, 0, result.stdout)
            result = self.run_command(
                CMAKE_COMMAND, "--build", str(build_dir), "--config", "Release"
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            prefix = root / "install"
            result = self.run_command(
                CMAKE_COMMAND,
                "--install",
                str(build_dir),
                "--config",
                "Release",
                "--prefix",
                str(prefix),
                "--component",
                "FixtureTests",
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            manifest = (
                (build_dir / "install_manifest_FixtureTests.txt")
                .read_text()
                .splitlines()
            )
            self.assertEqual(len(manifest), len(set(manifest)))

            relocated = root / "relocated install"
            prefix.rename(relocated)
            tests_dir = relocated / "share/tests"
            for destination, source in (
                ("fixture.txt", "fixture.txt"),
                ("other/fixture.txt", "fixture.txt"),
                ("tree/nested.txt", "tree/nested.txt"),
                ("other/tree/nested.txt", "tree/nested.txt"),
            ):
                self.assertEqual(
                    (tests_dir / "testdata" / destination).read_bytes(),
                    (FIXTURE_SOURCE_DIR / source).read_bytes(),
                )
            result = self.run_command(
                CTEST_COMMAND, "--test-dir", str(tests_dir), "--show-only=json-v1"
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            tests = json.loads(result.stdout)["tests"]
            self.assertEqual(
                {test["name"] for test in tests},
                {f"deferred/{index}" for index in range(16)}
                | {
                    "second-alias",
                    "existing-target",
                    "existing-alias",
                    "library-aliases",
                },
            )
            for test in tests:
                with self.subTest(test=test["name"]):
                    self.assertIn("command", test)
                    executable = Path(test["command"][0]).resolve()
                    self.assertTrue(executable.is_relative_to(relocated))
                    self.assertTrue(executable.is_file(), executable)
            result = self.run_command(
                CTEST_COMMAND, "--test-dir", str(tests_dir), "--output-on-failure"
            )
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_rejects_unresolved_deferred_target(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            result = self.configure(
                Path(temporary_dir) / "build", "-DIREE_TEST_DECLARE_TOOL=OFF"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Installed test target was referenced before it existed and was "
                "never created: first::tool",
                " ".join(result.stdout.split()),
            )

    def test_rejects_changed_deferred_artifact_path(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            result = self.configure(
                Path(temporary_dir) / "build", "-DIREE_TEST_TOOL_OUTPUT_NAME=renamed"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Installed test target 'first::tool' resolved to", result.stdout
            )
            self.assertIn(
                "after a test had already recorded", " ".join(result.stdout.split())
            )


if __name__ == "__main__":
    unittest.main()
