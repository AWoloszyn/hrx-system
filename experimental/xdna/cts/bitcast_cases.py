# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent byte oracles for scalar/vector bit casts on native XDNA."""

import sys
from pathlib import Path


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    inputs = bytearray()
    expected = bytearray()
    for record in range(6):
        # Each odd multiplier covers every byte pattern in every lane, while
        # adjacent lanes and successive packets carry different values.
        words = [
            bytes(
                (index * (2 * lane + 1) + 37 * record + 19 * lane) & 255
                for lane in range(4)
            )
            for index in range(256)
        ]
        for index, lanes in enumerate(words):
            inputs.extend(lanes)
            expected.extend(
                (value + position + 1) & 255
                for position, value in enumerate(reversed(lanes))
            )
            # FP16 and BF16 lane swaps preserve their original payload bits.
            expected.extend(lanes[2:] + lanes[:2])
            expected.extend(lanes[2:] + lanes[:2])
            expected.extend(lanes[position] for position in (1, 3, 0, 2))
            expected.extend(lanes[::-1])
            # The wide byte reversal crosses both scalar words and passes
            # through an F64 payload before the two output words are stored.
            expected.extend((lanes + words[(index + 1) & 255])[::-1])
            # Constexpr E5M2 signaling NaN and negative zero retain their bytes
            # when assembled with runtime floating lanes.
            expected.extend((0x7D, lanes[1], 0x80, lanes[3]))
    guard = bytes([0xA5]) * 64
    (directory / "input.bin").write_bytes(inputs + guard)
    (directory / "output.bin").write_bytes(bytes([0xCC]) * len(expected) + guard)
    (directory / "expected.bin").write_bytes(expected + guard)


if __name__ == "__main__":
    main()
