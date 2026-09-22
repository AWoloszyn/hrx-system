// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CTS_TRANSFER_TRIAL_H_
#define IREE_NET_CTS_TRANSFER_TRIAL_H_

#include "iree/async/cts/util/registry.h"
#include "iree/net/cts/transport_backend.h"

namespace iree::net::cts {

// Application reporting policy, independent of native completion batching.
enum class TransferProgressPolicy {
  // Report checked records immediately from the receive callback when admitted.
  kImmediate,
  // Merge checked positions and flush before the next blocking poll.
  kPollTurn,
};

// Fixed workload shared by transport correctness tests and benchmarks.
struct TransferTrialOptions {
  // Connections sharing one producer proactor and one consumer proactor.
  size_t connection_count = 1;
  // Checked bytes per record, excluding the queue envelope.
  size_t record_size = 64;
  // Maximum records of one timeline in each COMMAND message.
  size_t batch_size = 1;
  // Maximum submitted but unobserved records per independent timeline.
  size_t window_size = 32;
  // Nonempty borrowed SG fragments per COMMAND payload.
  size_t fragment_count = 1;
  // Records per timeline and connection completed before measurement.
  uint64_t warmup_records = 32;
  // Records per timeline and connection in the measured interval.
  uint64_t measured_records = 1024;
  // Application policy for sending completed-frontier ADVANCE messages.
  TransferProgressPolicy progress_policy = TransferProgressPolicy::kPollTurn;
};

// Measurements of the fixed workload, excluding setup, warm-up and teardown.
struct TransferTrialResult {
  // The requested stack created a listener; later errors are trial failures.
  bool available = false;
  // End-to-end interval through progress observation and producer source join.
  double elapsed_seconds = 0;
  // Total checked records across both timelines and all connections.
  uint64_t records = 0;
  // Total checked payload bytes, excluding protocol envelopes and feedback.
  uint64_t payload_bytes = 0;
  // Accepted producer COMMAND messages.
  uint64_t command_messages = 0;
  // Producer terminal source callbacks; must equal command_messages.
  uint64_t source_completions = 0;
  // Completed-frontier ADVANCE messages observed by the producer.
  uint64_t progress_messages = 0;
  // Maximum unobserved records on any one timeline during measurement.
  uint64_t window_high_water = 0;
  // Actual producer proactor capabilities after the named backend mask.
  iree_async_proactor_capabilities_t proactor_capabilities = 0;
};

// Runs a checked-transfer trial using real sessions and queue channels.
//
// Two application timelines per connection advance independently after payload
// validation. Each timeline is consumed synchronously in message order; that
// application contract establishes its completed prefix, not queue submission
// order. Reporting coalesces only those witnessed coordinates.
//
// The producer runs on the caller, the consumer on a dedicated test-application
// thread. Each owns its proactor throughout polling and cleanup. Borrowed SG
// sources are immutable until every accepted send completes. Timing ends only
// after producer observation and source retirement; consumer send callbacks and
// both session deactivations are also joined before returning. Errors drain
// accepted work and are returned instead of producing a successful timing row.
//
// This same-process host-memory trial does not qualify process isolation,
// registered device memory, native execution or DMA visibility.
iree_status_t RunTransferTrial(
    const TransportBackend& transport,
    const iree::async::cts::ProactorFactory& create_proactor,
    const TransferTrialOptions& options, TransferTrialResult* out_result);

}  // namespace iree::net::cts

#endif  // IREE_NET_CTS_TRANSFER_TRIAL_H_
