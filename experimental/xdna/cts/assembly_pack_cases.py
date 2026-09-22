# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent signed INT4 saturation and truncation oracles for XDNA."""

import sys
from pathlib import Path


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    inputs = bytearray()
    expected = bytearray()
    for record in range(6):
        # Every odd multiplier permutes all 256 byte patterns. Each worker sees
        # different neighboring lanes while exercising every saturation edge.
        values = [
            ((index * (2 * record + 1) + 37 * record) & 255) - 128
            for index in range(256)
        ]
        inputs.extend(value & 255 for value in values)
        expected.extend([0xA5] * 64)
        saturated = [min(7, max(-8, value)) & 15 for value in values]
        truncated = [value & 15 for value in values]
        for nibbles in [saturated, truncated]:
            expected.extend(
                nibbles[index] | (nibbles[index + 1] << 4) for index in range(0, 256, 2)
            )
        expected.extend([0xA5] * 64)
    # Binding tails complement the per-record guards written by the kernel.
    guard = bytes([0x5A] * 64)
    (directory / "input.bin").write_bytes(inputs + guard)
    (directory / "output.bin").write_bytes(bytes([0xCC] * len(expected)) + guard)
    (directory / "expected.bin").write_bytes(expected + guard)


if __name__ == "__main__":
    main()
