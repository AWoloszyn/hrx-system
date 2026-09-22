# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import (
    REPO_ROOT,
    configure_project,
    test_project,
)

FIXTURE = REPO_ROOT / "build_tools/cmake/testdata/test_metadata"


class CMakeTestMetadataTest(unittest.TestCase):
    def test_escaped_names_keep_ordered_unique_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            extension = root / "extension.cmake"
            name = 'quoted "name" \\ path\nwith\ttab'
            extension.write_text(
                f"set(_NAME [=[{name}]=])\n"
                'add_test(NAME "${_NAME}" COMMAND "${CMAKE_COMMAND}" -E true)\n'
                'iree_register_test_build_targets("${_NAME}"\n'
                "  TARGETS tool_backed_root host_root tool_backed_root)\n"
                'add_test(NAME OFF COMMAND "${CMAKE_COMMAND}" -E true)\n'
                "iree_register_test_build_targets(OFF)\n"
            )
            build = root / "build"
            configure_project(
                FIXTURE, build, f"-DIREE_TEST_METADATA_EXTENSION_FILE={extension}"
            )
            catalog = json.loads((build / "iree_ctest_build_targets.json").read_text())
            self.assertEqual(catalog["tests"][name], ["tool_backed_root", "host_root"])
            self.assertEqual(catalog["tests"]["OFF"], [])
            tests = json.loads(test_project(build, "--show-only=json-v1"))["tests"]
            self.assertIn(name, {test["name"] for test in tests})

    def test_rejects_invalid_build_metadata(self):
        cases = (
            ("", "missing IREE_BUILD_TARGETS metadata: OFF"),
            (
                "iree_register_test_build_targets(OFF TARGETS missing)",
                "missing IREE_BUILD_TARGETS target: missing",
            ),
            (
                "add_library(actual INTERFACE)\nadd_library(invalid ALIAS actual)\niree_register_test_build_targets(OFF TARGETS invalid)",
                "non-buildable alias",
            ),
            (
                "add_executable(invalid IMPORTED)\niree_register_test_build_targets(OFF TARGETS invalid)",
                "non-buildable imported",
            ),
            (
                "add_library(invalid INTERFACE)\niree_register_test_build_targets(OFF TARGETS invalid)",
                "non-buildable interface library",
            ),
            (
                "iree_register_test_build_targets(OFF)\niree_register_test_build_targets(OFF)",
                "duplicate IREE_BUILD_TARGETS metadata: OFF",
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            extension = root / "extension.cmake"
            for declarations, diagnostic in cases:
                with self.subTest(diagnostic=diagnostic):
                    extension.write_text(
                        'add_test(NAME OFF COMMAND "${CMAKE_COMMAND}" -E true)\n'
                        + declarations
                    )
                    output = configure_project(
                        FIXTURE,
                        root / "build",
                        f"-DIREE_TEST_METADATA_EXTENSION_FILE={extension}",
                        expect_failure=True,
                    )
                    self.assertIn(diagnostic, " ".join(output.split()))


if __name__ == "__main__":
    unittest.main()
