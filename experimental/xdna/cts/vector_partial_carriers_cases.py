# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Raw-byte oracle for captured and partial native vector carriers."""

import sys
from pathlib import Path


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    inputs = bytearray()
    expected = bytearray()
    for record in range(16):
        source = bytearray(
            ((record + 1) * 37 + position * 13) & 0xFF for position in range(256)
        )
        source[:4] = (1).to_bytes(4, "little")
        payload = source[64:192]
        result = bytearray([0xA5] * 512)
        result[0:128] = payload
        result[160:224] = payload[63:127]
        result[256:352] = payload[1:97]
        inputs += source
        expected += result
    guard = bytes([0xA5]) * 64
    (directory / "input.bin").write_bytes(inputs + guard)
    (directory / "output.bin").write_bytes(bytes([0xCD]) * len(expected) + guard)
    (directory / "expected.bin").write_bytes(expected + guard)


if __name__ == "__main__":
    main()
