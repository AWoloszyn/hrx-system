# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Checks XDNA source rejection at the compiler's output publication boundary."""

import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class PublicationTest(unittest.TestCase):
    def test_rejected_source_leaves_outputs_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "kernel.xdna"
            target_output = Path(directory) / "kernel.elf"
            for contents in (None, b"previous artifact"):
                with self.subTest(existing_output=contents is not None):
                    if contents is not None:
                        output.write_bytes(contents)
                        target_output.write_bytes(contents)
                    result = subprocess.run(
                        [
                            _ARGS.compiler,
                            _ARGS.source,
                            "--root=@copy_records",
                            "--target=amd.xdna.aie2p:amd.xdna.strix_halo.17f0_11",
                            f"--output={output}",
                            f"--emit-target-artifact={target_output}",
                        ],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("LOWERING/061", result.stderr)
                    self.assertEqual(result.stdout, "")
                    for artifact in (output, target_output):
                        if contents is None:
                            self.assertFalse(artifact.exists())
                        else:
                            self.assertEqual(artifact.read_bytes(), contents)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler")
    parser.add_argument("source")
    _ARGS = parser.parse_args()
    unittest.main(argv=[sys.argv[0]])
