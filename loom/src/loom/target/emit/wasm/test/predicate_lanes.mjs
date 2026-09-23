// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const binary = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(binary));
const {instance: {exports}} = await WebAssembly.instantiate(binary);

const values = [-2147483648, -1, 0, 1, 2147483647];
for (const a of values) {
  for (const b of values) {
    for (const c of values) {
      for (const d of values) {
        const inputs = [a, b, c, d];
        for (const limit of [-1, 0, 1]) {
          const sum = inputs.reduce((sum, value) => sum + Math.min(value, limit), 0) | 0;
          assert.deepEqual(exports.sum_capped_and_edges(...inputs, limit),
                           [sum, Number(a < limit), Number(d < limit)],
                           `${inputs}, limit=${limit}`);
        }
      }
    }
  }
}
