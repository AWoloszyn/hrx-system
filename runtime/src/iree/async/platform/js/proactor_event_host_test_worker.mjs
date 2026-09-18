// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import {parentPort, workerData} from 'node:worker_threads';

import {createImports} from './proactor_imports.mjs';

const imports = createImports({
  memory: null,
  ringBuffer: workerData.ringBuffer,
  ringCapacity: workerData.ringCapacity,
  cancelControl: new Int32Array(workerData.cancelBuffer),
  postMessage: () => {},
});

const results = [];
for (const token of workerData.tokens) {
  results.push(imports.timer_cancel(token));
}
parentPort.postMessage(results);
