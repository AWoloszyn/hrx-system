// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const {instance} = await WebAssembly.instantiate(readFileSync(process.argv[2]));
const exports = instance.exports;

for (const first of [0, 512, 2048]) {
  const second = first + 1024;
  const firstBytes = Uint8Array.from({length: 128}, (_, i) => (3 * i + 17) & 255);
  const secondBytes = Uint8Array.from({length: 128}, (_, i) => (7 * i + 93) & 255);
  for (let i = 0; i < firstBytes.length; ++i) {
    exports.write_byte(first, i, firstBytes[i]);
    exports.write_byte(second, i, secondBytes[i]);
  }

  for (const chooseFirst of [0, 1]) {
    assert.equal(exports.choose_buffer(first, second, chooseFirst),
                 chooseFirst ? first : second);
    const selected = chooseFirst ? firstBytes : secondBytes;
    for (const position of [0, 1, 31, 64, 127]) {
      const replacement = (position + 101) & 255;
      assert.equal(exports.exchange_selected_byte(first, second, chooseFirst,
                                                 position, replacement), selected[position]);
      selected[position] = replacement;
      assert.equal(exports.read_byte(first, position), firstBytes[position]);
      assert.equal(exports.read_byte(second, position), secondBytes[position]);
    }
    for (const firstOrigin of [0, 17, 64, 127]) {
      for (const secondOrigin of [0, 9, 32, 127]) {
        assert.equal(exports.selected_buffer_coordinate(first, second, firstOrigin,
                                                        secondOrigin, chooseFirst),
                     chooseFirst ? firstBytes[firstOrigin] : secondBytes[secondOrigin]);
      }
    }
  }

  for (const [firstOrigin, secondOrigin] of [[0, 127], [17, 64], [64, 9], [127, 0]]) {
    for (let count = 0; count <= 257; ++count) {
      assert.equal(exports.rotate_buffer_coordinates(first, second, firstOrigin,
                                                     secondOrigin, count),
                   count % 2 ? secondBytes[secondOrigin] : firstBytes[firstOrigin]);
    }
  }
}
