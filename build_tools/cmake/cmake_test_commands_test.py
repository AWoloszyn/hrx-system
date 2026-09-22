# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import (
    REPO_ROOT,
    build_project,
    configure_project,
    test_project,
)

FIXTURE = REPO_ROOT / "build_tools/cmake/testdata/test_commands"


class CMakeTestCommandsTest(unittest.TestCase):
    def test_target_execution_prefixes_and_arguments(self):
        with tempfile.TemporaryDirectory(prefix="cmake test commands ") as temporary:
            for cross_compile in ("OFF", "ON"):
                with self.subTest(cross_compile=cross_compile):
                    build = Path(temporary) / cross_compile
                    configure_project(
                        FIXTURE, build, f"-DIREE_TEST_CROSS_COMPILE={cross_compile}"
                    )
                    build_project(build, "fixture_native_test_deps")
                    test_project(build)


if __name__ == "__main__":
    unittest.main()
