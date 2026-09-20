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

const coordinates = [-2147483648, -2147483647, -1, 0, 1, 2147483646, 2147483647];
const offsets = [0, 1, 2147483647, 2147483648, 2147483649, 4294967294, 4294967295];
for (const coordinate of coordinates) {
  for (const offset of offsets) {
    assert.deepEqual(exports.widen_address_domains(coordinate, offset),
                     [BigInt(coordinate), BigInt(offset)],
                     `index ${coordinate}, offset ${offset}`);
  }
  assert.deepEqual(exports.widen_address_payload(coordinate),
                   [BigInt(coordinate), BigInt.asUintN(32, BigInt(coordinate))],
                   `payload ${coordinate}`);
}

assert.deepEqual(exports.widen_constant_addresses(),
                 [-2147483648n, -1n, 2147483648n, 4294967295n]);
