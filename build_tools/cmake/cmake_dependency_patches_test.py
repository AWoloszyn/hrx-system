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

CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
PATCH_DRIVER = Path(__file__).with_name("iree_apply_dependency_patches.cmake")
PATCH = """--- a/value.txt
+++ b/value.txt
@@ -1 +1 @@
-before
+after
"""


class CMakeDependencyPatchesTest(unittest.TestCase):
    def run_patch(self, source: Path, patch: Path) -> subprocess.CompletedProcess:
        git = shutil.which("git")
        self.assertIsNotNone(git, "dependency patching requires Git")
        return subprocess.run(
            [
                CMAKE_COMMAND,
                f"-DIREE_PATCH_GIT_EXECUTABLE={git}",
                f"-DIREE_PATCH_SOURCE_DIR={source}",
                f"-DIREE_PATCH_FILES={patch}",
                "-DIREE_PATCH_ARGS=-p1",
                "-P",
                str(PATCH_DRIVER),
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def test_patches_nested_population_and_accepts_exact_reapplication(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            source = root / "build" / "_deps" / "sample-src"
            source.mkdir(parents=True)
            value = source / "value.txt"
            value.write_text("before\n", encoding="utf-8")
            patch = root / "fix.patch"
            patch.write_text(PATCH, encoding="utf-8")
            for _ in range(2):
                result = self.run_patch(source, patch)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(value.read_text(encoding="utf-8"), "after\n")
            self.assertFalse((root / "value.txt").exists())

    def test_rejects_source_that_matches_neither_patch_direction(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            value = root / "value.txt"
            value.write_text("different\n", encoding="utf-8")
            patch = root / "fix.patch"
            patch.write_text(PATCH, encoding="utf-8")
            result = self.run_patch(root, patch)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("neither applies", result.stdout)
            self.assertEqual(value.read_text(encoding="utf-8"), "different\n")


if __name__ == "__main__":
    unittest.main()
