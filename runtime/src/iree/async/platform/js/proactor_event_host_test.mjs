// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {once} from 'node:events';
import {Worker} from 'node:worker_threads';

import {ProactorEventHost} from './proactor_event_host.mjs';
import {ProactorRing} from './proactor_ring.mjs';

const CANCEL_TOKEN = 0;
const CANCEL_RESPONSE = 1;
const CANCEL_REQUEST_STATE = 2;
const CANCEL_RESPONSE_PENDING = 0;
const CANCEL_RESPONSE_ALREADY_FIRED = 2;
const CANCEL_REQUEST_PENDING = 1;

const ringCapacity = 4;
const ringBuffer = ProactorRing.createSharedBuffer(ringCapacity);
const cancelBuffer = new SharedArrayBuffer(16);
const cancelControl = new Int32Array(cancelBuffer);
const eventHost =
    new ProactorEventHost(ringBuffer, cancelBuffer, ringCapacity);
eventHost.start();

let worker = null;
try {
  // WebAssembly i32 imports present these uint32_t bit patterns to JavaScript
  // as signed Numbers: 0, INT32_MAX, INT32_MIN, and UINT32_MAX.
  const tokens = [0, 0x7FFFFFFF, -0x80000000, -1];
  const futureDeadlineNs = process.hrtime.bigint() + 86_400_000_000_000n;
  for (const token of tokens) {
    eventHost.handleMessage(
        {type: 'timer_start', token, deadlineNs: futureDeadlineNs});
  }
  assert.equal(eventHost.timers.size, tokens.length);

  worker = new Worker(
      new URL('./proactor_event_host_test_worker.mjs', import.meta.url), {
        workerData: {
          tokens,
          ringBuffer,
          ringCapacity,
          cancelBuffer,
        },
      });
  const resultPromise = once(worker, 'message');
  const exitPromise = once(worker, 'exit');
  const [results] = await resultPromise;
  const [exitCode] = await exitPromise;

  assert.equal(exitCode, 0);
  assert.deepEqual(results, tokens.map(() => 1));
  assert.equal(eventHost.timers.size, 0);

  // A request published before shutdown must still receive a response so its
  // worker cannot remain blocked in Atomics.wait.
  Atomics.store(cancelControl, CANCEL_RESPONSE, CANCEL_RESPONSE_PENDING);
  Atomics.store(cancelControl, CANCEL_TOKEN, 42);
  Atomics.store(
      cancelControl, CANCEL_REQUEST_STATE, CANCEL_REQUEST_PENDING);
  eventHost.stop();
  await eventHost._cancelListenerPromise;
  assert.equal(
      Atomics.load(cancelControl, CANCEL_RESPONSE),
      CANCEL_RESPONSE_ALREADY_FIRED);
} finally {
  eventHost.stop();
  await eventHost._cancelListenerPromise;
  if (worker && worker.threadId !== -1) {
    await worker.terminate();
  }
}
