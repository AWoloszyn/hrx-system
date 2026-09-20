# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Executes the public compiler's Wasm artifact with Node.js."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class WasmArtifactTest(unittest.TestCase):
    def test_default_pipeline_executes_structured_source(self):
        compiler, source, oracle = sys.argv[1:]
        node = os.environ.get("IREE_WASM_NODE") or shutil.which("node")
        self.assertIsNotNone(node, "Install Node.js or set IREE_WASM_NODE")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "module.wasm"
            subprocess.run(
                [compiler, source, "--format=wasm-binary", f"--output={output}"],
                check=True,
            )
            subprocess.run([node, oracle, str(output)], check=True)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
