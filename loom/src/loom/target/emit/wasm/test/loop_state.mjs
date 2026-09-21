// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const binary = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(binary));
const {instance} = await WebAssembly.instantiate(binary);
const functions = instance.exports;

const ranges = [
  [0, 0, 1], [7, 3, 2], [0, 1, 1], [0, 19, 3], [-7, 8, 2],
  [2147483642, 2147483647, 2], [-2147483648, -2147483641, 3],
];
for (const [lower, upper, step] of ranges) {
  for (const seed of [0, 17, -39]) {
    let hash = seed;
    let last = seed;
    let left = seed;
    let right = seed + 100;
    let nested = seed;
    for (let index = lower; index < upper; index += step) {
      hash = (Math.imul(hash, 33) + index) | 0;
      last = index;
      [left, right] = [right, left];
      for (let inner = 2147483642; inner < 2147483647; inner += 2) {
        const chosen = (index >>> 0) < (inner >>> 0) ? index : inner;
        nested = (Math.imul(nested, 33) + chosen) | 0;
      }
    }
    assert.equal(functions.signed_hash(lower, upper, step, seed), hash);
    assert.equal(functions.last_iv(lower, upper, step, seed), last);
    assert.deepEqual(functions.swap(lower, upper, step, seed, seed + 100),
                     [left, right]);
    assert.equal(functions.nested(lower, upper, step, seed), nested);
  }
}
for (const [lower, upper, step] of [[0, 0, 1], [0, 17, 3],
                                   [4294967290, 4294967295, 2]]) {
  let hash = 17;
  for (let index = lower; index < upper; index += step) {
    hash = (Math.imul(hash, 33) + index) | 0;
  }
  assert.equal(functions.unsigned_hash(lower, upper, step, 17), hash);
}
assert.deepEqual(functions.while_swap(0, 13, 29), [13, 29]);
assert.deepEqual(functions.while_swap(1, 13, 29), [29, 13]);
assert.equal(functions.sparse_parameters(123n, 71, 3.5), 71);
