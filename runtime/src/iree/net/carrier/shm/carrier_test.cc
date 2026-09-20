// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/carrier.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "iree/async/proactor_platform.h"
#if !defined(IREE_PLATFORM_WINDOWS) && !defined(IREE_PLATFORM_WASM)
#include "iree/async/platform/posix/api.h"
#endif
#include "iree/net/carrier/shm/storage.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct ReceiveState {
  // All received bytes, copied before callback-scoped storage expires.
  std::vector<uint8_t> bytes;
  // Native leases explicitly moved out of callbacks.
  std::vector<iree_async_buffer_lease_t> leases;
  // Maximum native leases to retain; zero keeps the normal borrowed lifetime.
  size_t retention_limit = 0;
  // Number of callbacks with no movable native lease.
  size_t borrowed_count = 0;
  // Orderly send shutdown callbacks.
  size_t eof_count = 0;
  // Terminal errors delivered to this endpoint.
  size_t error_count = 0;
  // Most recently delivered error code.
  iree_status_code_t error_code = IREE_STATUS_OK;
  // Fixture marker proving callbacks run inside its poll invocation.
  bool* polling = nullptr;
  // Optional reentrant application behavior after receiving data.
  std::function<iree_status_t()> after_receive;
  // Optional reentrant application behavior after receiving a terminal error.
  std::function<void()> after_error;

  static iree_status_t Receive(void* user_data, iree_async_span_t span,
                               iree_async_buffer_lease_t* lease) {
    auto& self = *static_cast<ReceiveState*>(user_data);
    EXPECT_TRUE(*self.polling);
    if (!span.length) {
      EXPECT_EQ(lease, nullptr);
      ++self.eof_count;
      return iree_ok_status();
    }
    const uint8_t* bytes = iree_async_span_ptr(span);
    self.bytes.insert(self.bytes.end(), bytes, bytes + span.length);
    if (!lease) {
      ++self.borrowed_count;
    }
    if (lease && self.leases.size() < self.retention_limit) {
      self.leases.push_back(*lease);
      *lease = {};
    }
    return self.after_receive ? self.after_receive() : iree_ok_status();
  }

  static void Error(void* user_data, iree_status_t status) {
    auto& self = *static_cast<ReceiveState*>(user_data);
    EXPECT_TRUE(*self.polling);
    ++self.error_count;
    self.error_code = iree_status_code(status);
    iree_status_free(status);
    if (self.after_error) {
      self.after_error();
    }
  }
};

struct SendState {
  // Fixture marker proving callback affinity.
  bool* polling = nullptr;
  // Delivered terminal completion count.
  size_t count = 0;
  // Delivered terminal code.
  iree_status_code_t code = IREE_STATUS_UNKNOWN;
  // Published source bytes reported by the carrier.
  size_t bytes = 0;
  // Optional follow-on send or teardown in the completion callback.
  std::function<void()> after_complete;

  static void Complete(void* user_data, iree_status_t status,
                       iree_host_size_t bytes) {
    auto& self = *static_cast<SendState*>(user_data);
    EXPECT_TRUE(*self.polling);
    ++self.count;
    self.code = iree_status_code(status);
    self.bytes = bytes;
    iree_status_free(status);
    if (self.after_complete) {
      self.after_complete();
    }
  }
};

struct AdmissionAllocator {
  // Once set, any attempted transport allocation fails before admission.
  bool deny_allocations = false;
  // Number of attempted allocations after the gate closes.
  size_t denied_count = 0;

  static iree_status_t Control(void* user_data,
                               iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto& self = *static_cast<AdmissionAllocator*>(user_data);
    if (self.deny_allocations && (command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                                  command == IREE_ALLOCATOR_COMMAND_CALLOC)) {
      ++self.denied_count;
      *inout_ptr = nullptr;
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "transport allocation denied by test allocator");
    }
    iree_allocator_t allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, params, inout_ptr);
  }

  iree_allocator_t allocator() { return {this, Control}; }
};

class ShmCarrierTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
#if !defined(IREE_PLATFORM_WINDOWS) && !defined(IREE_PLATFORM_WASM)
    if (GetParam()) {
      IREE_ASSERT_OK(iree_async_proactor_create_posix(
          iree_async_proactor_options_default(), iree_allocator_system(),
          &proactor_));
      return;
    }
#endif
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor_));
  }

  void TearDown() override {
    Drain();
    for (auto& side : received_) {
      for (auto& receiver : side) {
        for (auto& lease : receiver.leases) {
          iree_async_buffer_lease_release(&lease);
        }
      }
    }
    ReleaseResources();
  }

  void Create(uint32_t slots = 4, uint32_t slot_capacity = 4096,
              uint32_t sends = 4, uint32_t prefix_capacity = 128,
              iree_allocator_t allocator = iree_allocator_system()) {
    iree_net_shm_region_layout_t layout;
    IREE_ASSERT_OK(iree_net_shm_region_calculate_layout(
        {2, slots, slot_capacity}, &layout));
    IREE_ASSERT_OK(iree_net_shm_storage_create(&layout, iree_allocator_system(),
                                               &storage_[0]));
    iree_async_primitive_t borrowed[IREE_NET_SHM_STORAGE_HANDLE_COUNT];
    iree_async_primitive_t handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT] = {};
    iree_net_shm_storage_export(storage_[0], borrowed);
    for (uint32_t i = 0; i < IREE_NET_SHM_STORAGE_HANDLE_COUNT; ++i) {
      IREE_ASSERT_OK(iree_async_primitive_dup(borrowed[i], &handles[i]));
    }
    IREE_ASSERT_OK(iree_net_shm_storage_import(
        &layout, handles, iree_allocator_system(), &storage_[1]));
    iree_net_shm_carrier_options_t options = {sends, prefix_capacity};
    for (uint32_t side = 0; side < 2; ++side) {
      iree_async_notification_shared_options_t notification_options = {};
      notification_options.epoch_address =
          iree_net_shm_region_epoch(storage_[side]->mapping.base, side);
      notification_options.wake_primitive =
          storage_[side]->wakes[side].wait_primitive;
      notification_options.signal_primitive =
          storage_[side]->wakes[side].signal_primitive;
      IREE_ASSERT_OK(iree_async_notification_create_shared(
          proactor_, &notification_options, &notifications_[side]));
      for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
        auto& carrier = carriers_[side][endpoint];
        IREE_ASSERT_OK(iree_net_shm_carrier_create(
            proactor_, storage_[side], notifications_[side], endpoint, &options,
            allocator, &carrier));
        received_[side][endpoint].polling = &polling_;
        IREE_ASSERT_OK(iree_net_carrier_set_handlers(
            carrier, {ReceiveState::Receive, ReceiveState::Error,
                      &received_[side][endpoint]}));
        IREE_ASSERT_OK(iree_net_carrier_activate(carrier));
      }
    }
  }

  template <typename Predicate>
  void PollUntil(Predicate predicate) {
    while (!predicate()) {
      polling_ = true;
      iree_status_t status =
          iree_async_proactor_poll(proactor_, iree_infinite_timeout(), nullptr);
      polling_ = false;
      IREE_ASSERT_OK(status);
    }
  }

  void Send(uint32_t side, uint32_t endpoint, std::vector<uint8_t>& bytes,
            SendState& result,
            iree_net_send_prefix_t prefix = iree_net_send_prefix_empty()) {
    result.polling = &polling_;
    iree_async_span_t span =
        iree_async_span_from_ptr(bytes.data(), bytes.size());
    iree_net_send_params_t params = {prefix,
                                     iree_async_span_list_make(&span, 1),
                                     {SendState::Complete, &result}};
    IREE_ASSERT_OK(iree_net_carrier_send(carriers_[side][endpoint], &params));
  }

  void Deactivate(uint32_t side, uint32_t endpoint) {
    if (!carriers_[side][endpoint] ||
        iree_net_carrier_state(carriers_[side][endpoint]) !=
            IREE_NET_CARRIER_STATE_ACTIVE) {
      return;
    }
    iree_net_carrier_deactivate(
        carriers_[side][endpoint],
        [](void* user_data) { *static_cast<bool*>(user_data) = true; },
        &drained_[side][endpoint]);
  }

  void Drain() {
    for (uint32_t side = 0; side < 2; ++side) {
      for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
        Deactivate(side, endpoint);
      }
    }
    PollUntil([&] {
      for (uint32_t side = 0; side < 2; ++side) {
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
          if (carriers_[side][endpoint] && !drained_[side][endpoint]) {
            return false;
          }
        }
      }
      return true;
    });
  }

  void ReleaseResources() {
    for (uint32_t side = 0; side < 2; ++side) {
      for (auto& carrier : carriers_[side]) {
        iree_net_carrier_release(carrier);
        carrier = nullptr;
      }
      iree_async_notification_release(notifications_[side]);
      notifications_[side] = nullptr;
      iree_net_shm_storage_release(storage_[side]);
      storage_[side] = nullptr;
    }
    iree_async_proactor_release(proactor_);
    proactor_ = nullptr;
  }

  // Actual platform executor, shared by these independently mapped peers.
  iree_async_proactor_t* proactor_ = nullptr;
  // Fixture-only callback-affinity marker.
  bool polling_ = false;
  // Independent native resource ownership for each side.
  iree_net_shm_storage_t* storage_[2] = {};
  // Exactly one polling consumer for each side's native wake.
  iree_async_notification_t* notifications_[2] = {};
  // Two independent endpoint pairs exercising shared notification fanout.
  iree_net_carrier_t* carriers_[2][2] = {};
  // Receive application state indexed by side and endpoint.
  ReceiveState received_[2][2];
  // Explicit drain completion flags indexed by side and endpoint.
  bool drained_[2][2] = {};
};

TEST_P(ShmCarrierTest, SegmentsAcrossSlotsAndResidentCapacity) {
  Create(2, 4097);
  for (size_t size : {size_t{1}, size_t{4096}, size_t{4097}, size_t{4098},
                      size_t{16389}, size_t{1024 * 1024}}) {
    std::vector<uint8_t> payload(size);
    for (size_t i = 0; i < size; ++i) {
      payload[i] = static_cast<uint8_t>(i);
    }
    SendState result;
    received_[1][0].bytes.clear();
    Send(0, 0, payload, result);
    PollUntil([&] { return result.count == 1; });
    EXPECT_EQ(result.code, IREE_STATUS_OK);
    EXPECT_EQ(result.bytes, size);
    EXPECT_EQ(received_[1][0].bytes, payload);
    std::fill(payload.begin(), payload.end(), 0xA5);
  }
}

TEST_P(ShmCarrierTest, FailureBeforeActivationIsStickyWithoutCallbacks) {
  iree_net_shm_region_layout_t layout;
  IREE_ASSERT_OK(iree_net_shm_region_calculate_layout({1, 2, 64}, &layout));
  IREE_ASSERT_OK(iree_net_shm_storage_create(&layout, iree_allocator_system(),
                                             &storage_[0]));
  iree_async_notification_shared_options_t notification_options = {};
  notification_options.epoch_address =
      iree_net_shm_region_epoch(storage_[0]->mapping.base, 0);
  notification_options.wake_primitive = storage_[0]->wakes[0].wait_primitive;
  notification_options.signal_primitive =
      storage_[0]->wakes[0].signal_primitive;
  IREE_ASSERT_OK(iree_async_notification_create_shared(
      proactor_, &notification_options, &notifications_[0]));
  iree_net_shm_carrier_options_t options =
      iree_net_shm_carrier_options_default();
  iree_net_carrier_t* carrier = nullptr;
  IREE_ASSERT_OK(
      iree_net_shm_carrier_create(proactor_, storage_[0], notifications_[0], 0,
                                  &options, iree_allocator_system(), &carrier));
  iree_net_shm_carrier_fail(
      carrier, iree_make_status(IREE_STATUS_UNAVAILABLE, "connection retired"));
  received_[0][0].polling = &polling_;
  IREE_ASSERT_OK(iree_net_carrier_set_handlers(
      carrier, {ReceiveState::Receive, ReceiveState::Error, &received_[0][0]}));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE,
                        iree_net_carrier_activate(carrier));
  EXPECT_EQ(iree_net_carrier_state(carrier), IREE_NET_CARRIER_STATE_CREATED);
  EXPECT_EQ(iree_net_carrier_pending_operation_count(carrier), 0);
  EXPECT_EQ(received_[0][0].error_count, 0u);
  iree_net_carrier_release(carrier);
}

TEST_P(ShmCarrierTest, PrefixAndScatterSourcesSpanMultipleResidentBatches) {
  Create(2, 37, 4, 16);
  std::vector<uint8_t> prefix(1025, 0xA1);
  std::array<std::array<uint8_t, 17>, IREE_NET_SHM_MAX_SEND_SPANS> sources;
  std::array<iree_async_span_t, IREE_NET_SHM_MAX_SEND_SPANS> spans;
  std::vector<uint8_t> expected(prefix);
  for (size_t i = 0; i < sources.size(); ++i) {
    sources[i].fill(static_cast<uint8_t>(i));
    spans[i] = iree_async_span_from_ptr(sources[i].data(), sources[i].size());
    expected.insert(expected.end(), sources[i].begin(), sources[i].end());
  }
  SendState result;
  result.polling = &polling_;
  iree_net_send_params_t params = {
      iree_net_send_prefix_from_bytes(
          iree_make_const_byte_span(prefix.data(), prefix.size())),
      iree_async_span_list_make(spans.data(), spans.size()),
      {SendState::Complete, &result}};
  IREE_ASSERT_OK(iree_net_carrier_send(carriers_[0][0], &params));
  std::fill(prefix.begin(), prefix.end(), 0xF1);
  PollUntil([&] { return result.count == 1; });
  EXPECT_EQ(result.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[1][0].bytes, expected);
}

TEST_P(ShmCarrierTest,
       RetainedSlotsDoNotDelaySourceCompletionOrOtherEndpoints) {
  Create(4, 32);
  received_[1][0].retention_limit = 100;
  std::vector<uint8_t> first(32 * 3, 0x31);
  SendState first_result;
  Send(0, 0, first, first_result);
  PollUntil([&] { return first_result.count == 1; });
  EXPECT_EQ(first_result.code, IREE_STATUS_OK);
  ASSERT_EQ(received_[1][0].leases.size(), 3u);
  std::fill(first.begin(), first.end(), 0x91);

  std::vector<uint8_t> later(2048, 0x42);
  std::vector<uint8_t> independent(1024, 0x53);
  SendState later_result;
  SendState independent_result;
  Send(0, 0, later, later_result);
  Send(0, 1, independent, independent_result);
  PollUntil(
      [&] { return later_result.count == 1 && independent_result.count == 1; });
  EXPECT_EQ(later_result.code, IREE_STATUS_OK);
  EXPECT_EQ(independent_result.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[1][1].bytes, independent);
  EXPECT_GT(received_[1][0].borrowed_count, 0u);
  for (auto& lease : received_[1][0].leases) {
    EXPECT_TRUE(std::all_of(iree_async_span_ptr(lease.span),
                            iree_async_span_ptr(lease.span) + lease.span.length,
                            [](uint8_t value) { return value == 0x31; }));
  }
  Drain();
  ReleaseResources();
  std::array<std::thread, 3> consumers;
  for (size_t i = 0; i < consumers.size(); ++i) {
    consumers[i] = std::thread([&, i] {
      iree_async_buffer_lease_release(&received_[1][0].leases[2 - i]);
    });
  }
  for (auto& consumer : consumers) {
    consumer.join();
  }
}

TEST_P(ShmCarrierTest, OneSlotProgressAlwaysBorrowsReceiveStorage) {
  Create(1, 31);
  received_[1][0].retention_limit = 100;
  std::vector<uint8_t> payload(5000, 0x63);
  SendState result;
  Send(0, 0, payload, result);
  PollUntil([&] { return result.count == 1; });
  EXPECT_EQ(result.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[1][0].bytes, payload);
  EXPECT_TRUE(received_[1][0].leases.empty());
  EXPECT_EQ(received_[1][0].borrowed_count, (payload.size() + 30) / 31);
}

TEST_P(ShmCarrierTest, CompletionReturnsAdmissionCapacityBeforeReentrantSend) {
  Create(2, 31, 1);
  std::vector<uint8_t> first(200, 0x51);
  std::vector<uint8_t> next(150, 0x62);
  SendState first_result;
  SendState next_result;
  first_result.after_complete = [&] {
    EXPECT_EQ(iree_net_carrier_query_send_budget(carriers_[0][0]).slots, 1u);
    Send(0, 0, next, next_result);
  };
  Send(0, 0, first, first_result);
  EXPECT_EQ(iree_net_carrier_query_send_budget(carriers_[0][0]).slots, 0u);
  bool writer_called = false;
  iree_net_send_params_t rejected = {{8,
                                      [](void* user_data, iree_byte_span_t) {
                                        *static_cast<bool*>(user_data) = true;
                                        return iree_ok_status();
                                      },
                                      &writer_called},
                                     iree_async_span_list_empty(),
                                     {SendState::Complete, &next_result}};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        iree_net_carrier_send(carriers_[0][0], &rejected));
  EXPECT_FALSE(writer_called);
  PollUntil([&] { return next_result.count == 1; });
  EXPECT_EQ(first_result.count, 1u);
  EXPECT_EQ(first_result.code, IREE_STATUS_OK);
  EXPECT_EQ(next_result.code, IREE_STATUS_OK);
  first.insert(first.end(), next.begin(), next.end());
  EXPECT_EQ(received_[1][0].bytes, first);
}

TEST_P(ShmCarrierTest, PrefixFailureCompletesLocallyWithoutPublishingBytes) {
  Create();
  SendState rejected;
  rejected.polling = &polling_;
  iree_net_send_params_t params = {{8,
                                    [](void*, iree_byte_span_t) {
                                      return iree_make_status(
                                          IREE_STATUS_ABORTED,
                                          "application prefix failure");
                                    },
                                    nullptr},
                                   iree_async_span_list_empty(),
                                   {SendState::Complete, &rejected}};
  IREE_ASSERT_OK(iree_net_carrier_send(carriers_[0][0], &params));
  std::vector<uint8_t> payload(12000, 0x78);
  SendState valid;
  Send(0, 0, payload, valid);
  PollUntil([&] { return valid.count == 1; });
  EXPECT_EQ(rejected.count, 1u);
  EXPECT_EQ(rejected.code, IREE_STATUS_ABORTED);
  EXPECT_EQ(rejected.bytes, 0u);
  EXPECT_EQ(received_[1][0].bytes, payload);
  EXPECT_EQ(received_[0][0].error_count, 0u);
}

TEST_P(ShmCarrierTest, CoalescesUnretainedSlotsIntoOneReceiptWake) {
  Create(16, 32);
  std::vector<uint8_t> payload(32 * 8, 0x61);
  SendState result;
  Send(0, 0, payload, result);
  PollUntil([&] { return result.count == 1; });
  EXPECT_EQ(result.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[1][0].bytes, payload);
  EXPECT_EQ(
      iree_atomic_load(iree_net_shm_region_epoch(storage_[0]->mapping.base, 0),
                       iree_memory_order_acquire),
      1);
}

TEST_P(ShmCarrierTest, SteadyStateNeedsNoCarrierAllocation) {
  AdmissionAllocator allocator;
  Create(2, 31, 4, 128, allocator.allocator());
  allocator.deny_allocations = true;
  std::array<uint8_t, 128> prefix;
  prefix.fill(0x91);
  std::vector<uint8_t> payload(1024 * 1024, 0x62);
  SendState result;
  Send(0, 0, payload, result,
       iree_net_send_prefix_from_bytes(
           iree_make_const_byte_span(prefix.data(), prefix.size())));
  PollUntil([&] { return result.count == 1; });
  EXPECT_EQ(result.code, IREE_STATUS_OK);
  EXPECT_EQ(result.bytes, prefix.size() + payload.size());
  EXPECT_EQ(allocator.denied_count, 0u);
  Drain();
  ReleaseResources();
}

TEST_P(ShmCarrierTest,
       OversizedPrefixAllocationFailureLeavesAdmissionUntouched) {
  AdmissionAllocator allocator;
  Create(2, 31, 1, 0, allocator.allocator());
  allocator.deny_allocations = true;
  SendState result;
  bool writer_called = false;
  iree_net_send_params_t params = {{8,
                                    [](void* user_data, iree_byte_span_t) {
                                      *static_cast<bool*>(user_data) = true;
                                      return iree_ok_status();
                                    },
                                    &writer_called},
                                   iree_async_span_list_empty(),
                                   {SendState::Complete, &result}};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        iree_net_carrier_send(carriers_[0][0], &params));
  EXPECT_FALSE(writer_called);
  EXPECT_EQ(result.count, 0u);
  EXPECT_EQ(allocator.denied_count, 1u);
  EXPECT_EQ(iree_net_carrier_query_send_budget(carriers_[0][0]).slots, 1u);
  std::vector<uint8_t> payload(8192, 0x63);
  Send(0, 0, payload, result);
  PollUntil([&] { return result.count == 1; });
  EXPECT_EQ(result.code, IREE_STATUS_OK);
  EXPECT_EQ(allocator.denied_count, 1u);
  Drain();
  ReleaseResources();
}

TEST_P(ShmCarrierTest, ConcurrentProducersPublishContiguousLogicalSends) {
  constexpr size_t kProducerCount = 16;
  constexpr size_t kPayloadSize = 32769;
  Create(4, 1024, kProducerCount);
  std::array<std::vector<uint8_t>, kProducerCount> payloads;
  std::array<SendState, kProducerCount> results;
  std::array<iree_status_t, kProducerCount> statuses = {};
  std::array<std::thread, kProducerCount> producers;
  for (size_t i = 0; i < kProducerCount; ++i) {
    payloads[i].assign(kPayloadSize, static_cast<uint8_t>(i));
    results[i].polling = &polling_;
    producers[i] = std::thread([&, i] {
      iree_async_span_t span =
          iree_async_span_from_ptr(payloads[i].data(), payloads[i].size());
      iree_net_send_params_t params = {iree_net_send_prefix_empty(),
                                       iree_async_span_list_make(&span, 1),
                                       {SendState::Complete, &results[i]}};
      statuses[i] = iree_net_carrier_send(carriers_[0][0], &params);
    });
  }
  PollUntil([&] {
    return std::all_of(
        results.begin(), results.end(),
        [](const SendState& result) { return result.count == 1; });
  });
  for (size_t i = 0; i < kProducerCount; ++i) {
    producers[i].join();
    IREE_ASSERT_OK(statuses[i]);
    EXPECT_EQ(results[i].code, IREE_STATUS_OK);
  }
  const auto& bytes = received_[1][0].bytes;
  ASSERT_EQ(bytes.size(), kProducerCount * kPayloadSize);
  std::array<bool, kProducerCount> seen = {};
  for (size_t start = 0; start < bytes.size(); start += kPayloadSize) {
    uint8_t producer = bytes[start];
    ASSERT_LT(producer, kProducerCount);
    EXPECT_FALSE(seen[producer]);
    seen[producer] = true;
    EXPECT_TRUE(std::all_of(bytes.begin() + start,
                            bytes.begin() + start + kPayloadSize,
                            [=](uint8_t value) { return value == producer; }));
  }
}

TEST_P(ShmCarrierTest, HalfClosePreservesReverseDirection) {
  Create(1, 19);
  std::vector<uint8_t> request(2000, 0x51);
  SendState request_result;
  Send(0, 0, request, request_result);
  IREE_ASSERT_OK(iree_net_carrier_shutdown(carriers_[0][0]));
  PollUntil([&] {
    return request_result.count == 1 && received_[1][0].eof_count == 1;
  });
  EXPECT_EQ(request_result.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[1][0].bytes, request);
  std::vector<uint8_t> response(2000, 0x62);
  SendState response_result;
  Send(1, 0, response, response_result);
  PollUntil([&] { return response_result.count == 1; });
  EXPECT_EQ(response_result.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[0][0].bytes, response);
  EXPECT_EQ(received_[0][0].error_count, 0u);
  EXPECT_EQ(received_[1][0].error_count, 0u);
}

TEST_P(ShmCarrierTest,
       EndpointRetirementFailsItsPeerWithoutClosingOtherEndpoints) {
  Create(2, 31);
  Deactivate(1, 0);
  std::vector<uint8_t> payload(10000, 0x53);
  SendState failed;
  Send(0, 0, payload, failed);
  SendState independent;
  Send(0, 1, payload, independent);
  PollUntil([&] {
    return drained_[1][0] && failed.count == 1 && independent.count == 1;
  });
  EXPECT_EQ(failed.code, IREE_STATUS_UNAVAILABLE);
  EXPECT_EQ(received_[0][0].error_count, 1u);
  EXPECT_EQ(independent.code, IREE_STATUS_OK);
  EXPECT_EQ(received_[1][1].bytes, payload);
  EXPECT_EQ(received_[0][1].error_count, 0u);
}

TEST_P(ShmCarrierTest, ReceiveFailureCanReentrantlyDeactivateAndRelease) {
  Create();
  std::function<void()> drained = [&] {
    EXPECT_TRUE(polling_);
    drained_[1][0] = true;
    iree_net_carrier_release(carriers_[1][0]);
    carriers_[1][0] = nullptr;
  };
  received_[1][0].after_receive = [] {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "application rejected input");
  };
  received_[1][0].after_error = [&] {
    iree_net_carrier_deactivate(
        carriers_[1][0],
        [](void* user_data) {
          (*static_cast<std::function<void()>*>(user_data))();
        },
        &drained);
  };
  std::vector<uint8_t> payload(20000, 0x41);
  SendState result;
  Send(0, 0, payload, result);
  PollUntil([&] { return drained_[1][0] && result.count == 1; });
  EXPECT_EQ(received_[1][0].error_count, 1u);
  EXPECT_EQ(received_[1][0].error_code, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(result.code, IREE_STATUS_UNAVAILABLE);
}

TEST_P(ShmCarrierTest, DeactivationJoinsConcurrentPrefixWriter) {
  Create();
  struct Gate {
    // Protects writer entry and release.
    std::mutex mutex;
    // Explicit writer rendezvous, independent of wall-clock time.
    std::condition_variable condition;
    // True after the accepted writer enters.
    bool entered = false;
    // True when the test permits the writer to finish.
    bool released = false;
  } gate;
  SendState result;
  result.polling = &polling_;
  iree_net_send_params_t params = {
      {1024,
       [](void* user_data, iree_byte_span_t target) {
         auto& gate = *static_cast<Gate*>(user_data);
         std::unique_lock<std::mutex> lock(gate.mutex);
         gate.entered = true;
         gate.condition.notify_all();
         gate.condition.wait(lock, [&] { return gate.released; });
         std::memset(target.data, 0x47, target.data_length);
         return iree_ok_status();
       },
       &gate},
      iree_async_span_list_empty(),
      {SendState::Complete, &result}};
  std::thread producer(
      [&] { IREE_EXPECT_OK(iree_net_carrier_send(carriers_[0][0], &params)); });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.condition.wait(lock, [&] { return gate.entered; });
  }
  Deactivate(0, 0);
  // The peer's terminal callback proves the close handoff ran, while the
  // accepted prefix writer must still keep local drain incomplete.
  PollUntil([&] { return received_[1][0].error_count == 1; });
  EXPECT_FALSE(drained_[0][0]);
  EXPECT_EQ(result.count, 0u);
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
  }
  gate.condition.notify_all();
  producer.join();
  PollUntil([&] { return drained_[0][0]; });
  EXPECT_EQ(result.count, 1u);
  EXPECT_EQ(result.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(result.bytes, 0u);
}

#if !defined(IREE_PLATFORM_WINDOWS) && !defined(IREE_PLATFORM_WASM)
INSTANTIATE_TEST_SUITE_P(Executors, ShmCarrierTest,
                         ::testing::Values(false, true));
#else
INSTANTIATE_TEST_SUITE_P(Executors, ShmCarrierTest, ::testing::Values(false));
#endif

}  // namespace
