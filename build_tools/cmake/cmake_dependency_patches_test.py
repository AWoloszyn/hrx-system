# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import configured_cmake_arguments

CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
PATCH_DRIVER = Path(__file__).with_name("iree_apply_dependency_patches.cmake")
FETCH_HELPERS = Path(__file__).with_name("iree_third_party_helpers.cmake")
PATCH = """--- a/value.txt
+++ b/value.txt
@@ -1 +1 @@
-before
+after
"""


class CMakeDependencyPatchesTest(unittest.TestCase):
    def test_fetch_content_preserves_all_patches_and_options(self):
        with tempfile.TemporaryDirectory(
            prefix="cmake patch fixture "
        ) as temporary_dir:
            root = Path(temporary_dir)
            source = root / "archive"
            source.mkdir()
            patches = root / "patch files"
            patches.mkdir()
            (source / "value.txt").write_text(
                "context    line\nbefore\n", encoding="utf-8"
            )
            for name, before, after in (
                ("first", "before", "middle"),
                ("second", "middle", "after"),
            ):
                (patches / f"{name}.patch").write_text(
                    PATCH.replace("-before", f"-{before}")
                    .replace("+after", f"+{after}")
                    .replace("@@ -1 +1 @@", "@@ -1,2 +1,2 @@\n context line"),
                    encoding="utf-8",
                )
            archive = root / "source.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                output.add(source, arcname="source")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            driver = root / "build_tools/cmake" / PATCH_DRIVER.name
            driver.parent.mkdir(parents=True)
            shutil.copyfile(PATCH_DRIVER, driver)
            (root / "CMakeLists.txt").write_text(
                f"""cmake_minimum_required(VERSION 3.26)
project(patch_test NONE)
set(IREE_ROOT_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")
include("{FETCH_HELPERS.as_posix()}")
set(IREE_DEP_SAMPLE_URLS "{archive.as_posix()}")
set(IREE_DEP_SAMPLE_SHA256 "{digest}")
set(IREE_DEP_SAMPLE_PATCHES
  "//patch files:first.patch" "//patch files:second.patch")
set(IREE_DEP_SAMPLE_PATCH_ARGS "-p1" "--ignore-space-change")
iree_populate_locked_fetch_content(sample sample_source)
""",
                encoding="utf-8",
            )
            build = root / "build"
            for _ in range(2):
                result = subprocess.run(
                    [
                        CMAKE_COMMAND,
                        "-S",
                        str(root),
                        "-B",
                        str(build),
                        *configured_cmake_arguments(),
                    ],
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                self.assertEqual(result.returncode, 0, result.stdout)
                cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
                self.assertIn(
                    f"CMAKE_GENERATOR:INTERNAL={os.environ['IREE_TEST_CMAKE_GENERATOR']}\n",
                    cache,
                )
                value = build / "_deps/sample-src/value.txt"
                self.assertEqual(
                    value.read_text(encoding="utf-8"), "context    line\nafter\n"
                )

    def run_patch(self, source: Path, patch: Path) -> subprocess.CompletedProcess:
        git = shutil.which("git")
        self.assertIsNotNone(git, "dependency patching requires Git")
        return subprocess.run(
            [
                CMAKE_COMMAND,
                f"-DIREE_PATCH_GIT_EXECUTABLE={git}",
                f"-DIREE_PATCH_SOURCE_DIR={source}",
                f"-DIREE_PATCH_FILE={patch}",
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

    def test_patch_stack_can_modify_a_file_created_by_an_earlier_patch(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            patch = root / "stack.patch"
            patch.write_text(
                "diff --git a/value.txt b/value.txt\n"
                "new file mode 100644\n"
                "--- /dev/null\n+++ b/value.txt\n"
                "@@ -0,0 +1 @@\n+before\n" + PATCH,
                encoding="utf-8",
            )
            for _ in range(2):
                result = self.run_patch(root, patch)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertEqual(
                    (root / "value.txt").read_text(encoding="utf-8"), "after\n"
                )

    def test_failed_later_patch_does_not_publish_earlier_patches(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            value = root / "value.txt"
            value.write_text("before\n", encoding="utf-8")
            patch = root / "stack.patch"
            patch.write_text(
                PATCH + PATCH.replace("-before", "-different"), encoding="utf-8"
            )
            result = self.run_patch(root, patch)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(value.read_text(encoding="utf-8"), "before\n")


if __name__ == "__main__":
    unittest.main()
