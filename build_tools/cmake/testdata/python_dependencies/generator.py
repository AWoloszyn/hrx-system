# Copyright 2026 The IREE Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
from pathlib import Path

from choice import CHOICE
from value import VALUE

parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True)
arguments = parser.parse_args()
arguments.output.write_text(f"{CHOICE}: {VALUE}\n", encoding="utf-8")
