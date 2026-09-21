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
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import bazel_to_cmake_converter
import bazel_to_cmake_targets

from build_tools.cmake.test_environment import configured_cmake_arguments

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/test_arguments"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CTEST_COMMAND = os.environ["IREE_TEST_CTEST_COMMAND"]
CONFIGURATION = os.environ.get("IREE_TEST_CMAKE_BUILD_TYPE") or "Release"


class FixtureBuildFileFunctions(bazel_to_cmake_converter.BuildFileFunctions):
    def _should_emit_python_target(self):
        return True


class CMakeTestArgumentsTest(unittest.TestCase):
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

    def configure_arguments(self, source: Path, build: Path) -> list[str]:
        return [
            CMAKE_COMMAND,
            "-S",
            str(source),
            "-B",
            str(build),
            *configured_cmake_arguments(),
            f"-DIREE_REPO_ROOT={REPO_ROOT}",
            f"-DPython3_EXECUTABLE={sys.executable}",
        ]

    def test_file_identity_survives_generation_and_relocation(self):
        with tempfile.TemporaryDirectory(prefix="cmake test arguments ") as temporary:
            root = Path(temporary)
            source = root / "source with spaces"
            build = root / "build with spaces"
            shutil.copytree(FIXTURE_SOURCE_DIR, source)
            runner_directory = source / "build_tools/testing"
            runner_directory.mkdir(parents=True)
            for name in ("execution.py", "execution_main.py"):
                shutil.copy2(REPO_ROOT / "build_tools/testing" / name, runner_directory)

            # Exercise repository-relative inputs declared in a subdirectory
            # through the real converter and its project-root convention.
            package = source / "converted"
            package.mkdir()
            converter = SimpleNamespace(body="")
            functions = FixtureBuildFileFunctions(
                converter=converter,
                targets=bazel_to_cmake_targets.TargetConverter(repo_map={"@hrx": ""}),
                build_dir=str(package),
                repo_root=str(source),
            )
            functions.filegroup(
                name="inputs", srcs=["//:fixture.txt", "//:fixture.txt.more"]
            )
            functions.iree_py_test(
                name="converted_python",
                srcs=["//:reader.py", "//:reader_helper.py"],
                main="reader.py",
                args=["$(location //tools:runner)", "$(locations :inputs)"],
                data=[":inputs", "//tools:runner"],
                package_dirs=["${PROJECT_SOURCE_DIR}"],
            )
            (package / "CMakeLists.txt").write_text(converter.body)
            self.run_command(*self.configure_arguments(source, build))
            build_command = [
                CMAKE_COMMAND,
                "--build",
                str(build),
                "--config",
                CONFIGURATION,
                "--parallel",
                "4",
            ]
            ctest_command = [
                CTEST_COMMAND,
                "--test-dir",
                str(build),
                "--build-config",
                CONFIGURATION,
            ]
            self.run_command(*build_command)
            self.run_command(*ctest_command, "--output-on-failure", "--no-tests=error")
            tests = json.loads(self.run_command(*ctest_command, "--show-only=json-v1"))
            converted = next(
                test
                for test in tests["tests"]
                if test["name"] == "fixture/converted/converted_python"
            )
            self.assertEqual(
                [Path(argument) for argument in converted["command"][-2:]],
                [source / "fixture.txt", source / "fixture.txt.more"],
            )

            generated = [build / "generated.py", build / "generated.txt"]
            modification_times = [path.stat().st_mtime_ns for path in generated]
            self.run_command(*build_command)
            self.assertEqual(
                [path.stat().st_mtime_ns for path in generated], modification_times
            )
            with (source / "reader.py").open("a") as output:
                output.write('\nprint("updated Python source")\n')
            with (source / "fixture.txt").open("a") as output:
                output.write("updated input\n")
            self.run_command(*build_command)
            self.assertEqual(
                generated[0].read_bytes(), (source / "reader.py").read_bytes()
            )
            self.assertEqual(
                generated[1].read_bytes(), (source / "fixture.txt").read_bytes()
            )
            for path in generated:
                path.unlink()
            self.run_command(*build_command)
            self.assertIn(
                "updated Python source", self.run_command(*ctest_command, "--verbose")
            )

            prefix = root / "install"
            self.run_command(
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
            self.run_command(
                CTEST_COMMAND,
                "--test-dir",
                str(relocated / "share/tests"),
                "--build-config",
                CONFIGURATION,
                "--output-on-failure",
                "--no-tests=error",
            )

    def test_malformed_locators_fail_at_registration(self):
        for argument in ("--input={{}}", "--input={{missing"):
            with (
                self.subTest(argument=argument),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                source = root / "source"
                shutil.copytree(FIXTURE_SOURCE_DIR, source)
                result = subprocess.run(
                    [
                        *self.configure_arguments(source, root / "build"),
                        f"-DIREE_TEST_INVALID_ARGUMENT={argument}",
                    ],
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("Invalid file locator in test argument", result.stdout)


if __name__ == "__main__":
    unittest.main()
