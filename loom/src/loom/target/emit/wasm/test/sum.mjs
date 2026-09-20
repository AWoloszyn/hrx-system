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
assert.deepEqual(Object.keys(instance.exports), ['sum_to']);
for (let end = 0; end <= 1024; ++end) {
  const expected = Math.max(0, end * (end - 1) / 2);
  assert.equal(instance.exports.sum_to(end), expected, `sum_to(${end})`);
}
