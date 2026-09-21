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
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/data_dependencies"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CTEST_COMMAND = os.environ["IREE_TEST_CTEST_COMMAND"]
CONFIGURATION = os.environ.get("IREE_TEST_CMAKE_BUILD_TYPE") or "Release"


def configure_fixture(build_dir: Path, *cmake_args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            CMAKE_COMMAND,
            "-S",
            str(FIXTURE_SOURCE_DIR),
            "-B",
            str(build_dir),
            *configured_cmake_arguments(),
            f"-DIREE_REPO_ROOT={REPO_ROOT}",
            *cmake_args,
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


class CMakeDataDependenciesTest(unittest.TestCase):
    def test_shared_data_keeps_file_identity_and_stays_fresh(self):
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

        with tempfile.TemporaryDirectory(prefix="cmake staged data ") as temporary:
            root = Path(temporary)
            source = root / "source with spaces"
            build = root / "build with spaces"
            shutil.copytree(FIXTURE_SOURCE_DIR.parent / "data_staging", source)
            run(
                CMAKE_COMMAND,
                "-S",
                str(source),
                "-B",
                str(build),
                *configured_cmake_arguments(),
                f"-DIREE_REPO_ROOT={REPO_ROOT}",
            )
            build_command = (
                CMAKE_COMMAND,
                "--build",
                str(build),
                "--config",
                CONFIGURATION,
                "--parallel",
                "4",
            )
            run(*build_command)
            paths = [build / name for name in ("shared.txt", "a/b.txt", "a_b.txt")]
            modification_times = [path.stat().st_mtime_ns for path in paths]
            run(*build_command)
            self.assertEqual(
                [path.stat().st_mtime_ns for path in paths], modification_times
            )

            (source / "shared.txt").write_text("updated shared payload\n")
            run(*build_command)
            self.assertEqual(paths[0].read_text(), "updated shared payload\n")
            self.assertEqual(
                [path.stat().st_mtime_ns for path in paths[1:]], modification_times[1:]
            )

            # Even an unchanged input's newer timestamp must be consumed once.
            # A conditional copy would leave the output stale and rerun forever.
            (source / "shared.txt").touch()
            run(*build_command)
            self.assertGreaterEqual(
                paths[0].stat().st_mtime_ns, (source / "shared.txt").stat().st_mtime_ns
            )
            modification_times = [path.stat().st_mtime_ns for path in paths]
            run(*build_command)
            self.assertEqual(
                [path.stat().st_mtime_ns for path in paths], modification_times
            )

            paths[1].unlink()
            run(*build_command)
            for path in paths:
                self.assertEqual(
                    path.read_bytes(), (source / path.relative_to(build)).read_bytes()
                )
            output = run(
                CTEST_COMMAND,
                "--test-dir",
                str(build),
                "--build-config",
                CONFIGURATION,
                "--verbose",
            )
            for payload in (
                "updated shared payload",
                "slash payload",
                "underscore payload",
            ):
                self.assertIn(payload, output)

            prefix = root / "install"
            run(
                CMAKE_COMMAND,
                "--install",
                str(build),
                "--config",
                CONFIGURATION,
                "--prefix",
                str(prefix),
                "--component",
                "FixtureTests",
            )
            relocated = root / "relocated install"
            prefix.rename(relocated)
            source.rename(root / "retired source")
            build.rename(root / "retired build")
            output = run(
                CTEST_COMMAND,
                "--test-dir",
                str(relocated / "share/tests"),
                "--build-config",
                CONFIGURATION,
                "--verbose",
            )
            for payload in (
                "updated shared payload",
                "slash payload",
                "underscore payload",
            ):
                self.assertIn(payload, output)

    def test_resolves_later_target_and_copies_file_data(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            build_dir = Path(temporary_dir) / "build"
            configure_result = configure_fixture(build_dir)
            self.assertEqual(
                configure_result.returncode,
                0,
                msg=configure_result.stdout,
            )

            build_result = subprocess.run(
                [
                    CMAKE_COMMAND,
                    "--build",
                    str(build_dir),
                    "--target",
                    "data_consumer",
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(build_result.returncode, 0, msg=build_result.stdout)
            self.assertEqual(
                (build_dir / "fixture.txt").read_text(encoding="utf-8"),
                "fixture data\n",
            )
            self.assertEqual(
                (build_dir / "generated.txt").read_text(encoding="utf-8"),
                "fixture data\n",
            )
            self.assertTrue(build_dir.joinpath("tool-built.marker").is_file())

    def test_target_file_locator_builds_its_producer(self):
        for reference in ("fixture::tool", "data_tool"):
            with (
                self.subTest(reference=reference),
                tempfile.TemporaryDirectory() as temporary_dir,
            ):
                build_dir = Path(temporary_dir) / "build"
                configure_result = configure_fixture(
                    build_dir, f"-DIREE_TEST_TOOL_REFERENCE={reference}"
                )
                self.assertEqual(
                    configure_result.returncode, 0, configure_result.stdout
                )
                build_result = subprocess.run(
                    [
                        CMAKE_COMMAND,
                        "--build",
                        str(build_dir),
                        "--target",
                        "tool_path_consumer",
                    ],
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                self.assertEqual(build_result.returncode, 0, build_result.stdout)
                self.assertTrue(build_dir.joinpath("tool-built.marker").is_file())

    def test_rejects_missing_target_with_consumer_context(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            build_dir = Path(temporary_dir) / "build"
            configure_result = configure_fixture(
                build_dir,
                "-DIREE_TEST_DECLARE_TOOL=OFF",
            )

            self.assertNotEqual(configure_result.returncode, 0)
            self.assertIn(
                "IREE target data_consumer depends on missing target: fixture::tool",
                configure_result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
