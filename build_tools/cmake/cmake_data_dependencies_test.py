# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import (
    REPO_ROOT,
    build_project,
    configure_project,
    install_project,
    test_project,
)

FIXTURES = REPO_ROOT / "build_tools/cmake/testdata"


class CMakeDataDependenciesTest(unittest.TestCase):
    def test_shared_staged_data_stays_fresh_and_relocates(self):
        with tempfile.TemporaryDirectory(prefix="cmake staged data ") as temporary:
            root = Path(temporary)
            source, build = root / "source with spaces", root / "build with spaces"
            shutil.copytree(FIXTURES / "data_staging", source)
            configure_project(source, build)
            build_project(build)
            paths = [build / name for name in ("shared.txt", "a/b.txt", "a_b.txt")]
            timestamps = [path.stat().st_mtime_ns for path in paths]
            build_project(build)
            self.assertEqual([path.stat().st_mtime_ns for path in paths], timestamps)

            (source / "shared.txt").write_text("updated shared payload\n")
            build_project(build)
            self.assertEqual(paths[0].read_text(), "updated shared payload\n")
            self.assertEqual(
                [path.stat().st_mtime_ns for path in paths[1:]], timestamps[1:]
            )
            # A newer input with identical contents must not leave its copy stale.
            (source / "shared.txt").touch()
            build_project(build)
            self.assertGreaterEqual(
                paths[0].stat().st_mtime_ns, (source / "shared.txt").stat().st_mtime_ns
            )
            timestamps = [path.stat().st_mtime_ns for path in paths]
            build_project(build)
            self.assertEqual([path.stat().st_mtime_ns for path in paths], timestamps)
            paths[1].unlink()
            build_project(build)
            for path in paths:
                self.assertEqual(
                    path.read_bytes(), (source / path.relative_to(build)).read_bytes()
                )
            test_project(build)

            prefix = root / "install"
            install_project(build, prefix)
            relocated = root / "relocated install"
            prefix.rename(relocated)
            source.rename(root / "retired source")
            build.rename(root / "retired build")
            output = test_project(relocated / "share/tests", "--verbose")
            for payload in (
                "updated shared payload",
                "slash payload",
                "underscore payload",
            ):
                self.assertIn(payload, output)

    def test_file_and_forward_target_data_build_their_producers(self):
        with tempfile.TemporaryDirectory() as temporary:
            for index, (target, reference) in enumerate(
                (
                    ("data_consumer", "fixture::tool"),
                    ("tool_path_consumer", "fixture::tool"),
                    ("tool_path_consumer", "data_tool"),
                )
            ):
                with self.subTest(target=target, reference=reference):
                    build = Path(temporary) / str(index)
                    configure_project(
                        FIXTURES / "data_dependencies",
                        build,
                        f"-DIREE_TEST_TOOL_REFERENCE={reference}",
                    )
                    build_project(build, target)
                    self.assertTrue((build / "tool-built.marker").is_file())
                    if target == "data_consumer":
                        for name in ("fixture.txt", "generated.txt"):
                            self.assertEqual(
                                (build / name).read_text(), "fixture data\n"
                            )

    def test_missing_target_reports_the_consumer(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = configure_project(
                FIXTURES / "data_dependencies",
                Path(temporary),
                "-DIREE_TEST_DECLARE_TOOL=OFF",
                expect_failure=True,
            )
            self.assertIn(
                "data_consumer depends on missing target: fixture::tool", output
            )


if __name__ == "__main__":
    unittest.main()
