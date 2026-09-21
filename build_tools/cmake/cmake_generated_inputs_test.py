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

from build_tools.cmake.test_environment import configured_cmake_arguments

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_SOURCE_DIR = Path(__file__).resolve().parent / "testdata/generated_inputs"
CMAKE_COMMAND = os.environ["IREE_TEST_CMAKE_COMMAND"]
CONFIGURATION = os.environ.get("IREE_TEST_CMAKE_BUILD_TYPE") or "Release"


class CMakeGeneratedInputsTest(unittest.TestCase):
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

    def test_forward_generated_inputs_build_and_stay_fresh(self):
        for rule in (
            "library",
            "alwayslink",
            "interface",
            "binary",
            "test",
            "benchmark",
            "fuzz",
        ):
            inputs = (
                ("header", "textual") if rule == "interface" else ("header", "source")
            )
            for input_kind in inputs:
                with (
                    self.subTest(rule=rule, input_kind=input_kind),
                    tempfile.TemporaryDirectory(
                        prefix="cmake generated inputs "
                    ) as temporary_dir,
                ):
                    source_dir = Path(temporary_dir) / "source with spaces"
                    build_dir = Path(temporary_dir) / "build with spaces"
                    shutil.copytree(FIXTURE_SOURCE_DIR, source_dir)
                    self.run_command(
                        CMAKE_COMMAND,
                        "-S",
                        str(source_dir),
                        "-B",
                        str(build_dir),
                        *configured_cmake_arguments(),
                        f"-DIREE_REPO_ROOT={REPO_ROOT}",
                        f"-DIREE_TEST_RULE={rule}",
                        f"-DIREE_TEST_INPUT={input_kind}",
                    )
                    if (build_dir / "libfuzzer-unavailable.txt").exists():
                        self.skipTest(
                            "The configured compiler has no libFuzzer runtime"
                        )
                    target, program_path, generated_path = (
                        (build_dir / f"paths-{CONFIGURATION}.txt")
                        .read_text()
                        .splitlines()
                    )
                    program = Path(program_path)
                    generated = Path(generated_path)

                    def build_and_run(expected: int):
                        self.run_command(
                            CMAKE_COMMAND,
                            "--build",
                            str(build_dir),
                            "--config",
                            CONFIGURATION,
                            "--parallel",
                            "4",
                            "--target",
                            target,
                        )
                        output = self.run_command(
                            str(program), *(["-runs=1"] if rule == "fuzz" else [])
                        )
                        if rule == "fuzz":
                            self.assertIn(f"value={expected}\n", output)
                        else:
                            self.assertEqual(output, f"value={expected}\n")

                    build_and_run(7)
                    modification_time = program.stat().st_mtime_ns
                    build_and_run(7)
                    self.assertEqual(program.stat().st_mtime_ns, modification_time)

                    (source_dir / "value.txt").write_text("11\n")
                    build_and_run(11)
                    main_source = source_dir / (
                        "fuzz.cc" if rule == "fuzz" else "main.c"
                    )
                    main_source.write_text(
                        main_source.read_text().replace(
                            "fixture_value()", "fixture_value() + 1"
                        )
                    )
                    build_and_run(12)
                    generated.unlink()
                    build_and_run(12)


if __name__ == "__main__":
    unittest.main()
