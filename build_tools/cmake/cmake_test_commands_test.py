# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import configured_cmake_arguments

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/test_commands"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CTEST_COMMAND = os.environ["IREE_TEST_CTEST_COMMAND"]
CONFIGURATION = os.environ.get("IREE_TEST_CMAKE_BUILD_TYPE") or "Release"


class CMakeTestCommandsTest(unittest.TestCase):
    def check_commands(self, cross_compile: bool):
        def run(*arguments: str) -> str:
            result = subprocess.run(
                arguments,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            return result.stdout

        with tempfile.TemporaryDirectory(prefix="cmake test commands ") as temporary:
            build = Path(temporary) / "build with spaces"
            run(
                CMAKE_COMMAND,
                "-S",
                str(FIXTURE_SOURCE_DIR),
                "-B",
                str(build),
                *configured_cmake_arguments(),
                f"-DIREE_REPO_ROOT={REPO_ROOT}",
                f"-DIREE_TEST_CROSS_COMPILE={'ON' if cross_compile else 'OFF'}",
            )
            run(
                CMAKE_COMMAND,
                "--build",
                str(build),
                "--config",
                CONFIGURATION,
                "--parallel",
                "4",
                "--target",
                "fixture_native_test_deps",
            )
            ctest_command = (
                CTEST_COMMAND,
                "--test-dir",
                str(build),
                "--build-config",
                CONFIGURATION,
            )
            tests = json.loads(run(*ctest_command, "--show-only=json-v1"))["tests"]
            self.assertEqual(
                {test["name"] for test in tests}, {"fixture/native", "fixture/compiled"}
            )
            run(*ctest_command, "--output-on-failure", "--no-tests=error")

    def test_native_commands(self):
        self.check_commands(cross_compile=False)

    def test_target_execution_prefixes(self):
        self.check_commands(cross_compile=True)


if __name__ == "__main__":
    unittest.main()
