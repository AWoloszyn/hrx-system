// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/util/proactor_pool.h"

#include <array>
#include <cstring>
#include <thread>

#include "iree/async/operations/scheduling.h"
#include "iree/base/internal/atomics.h"
#include "iree/base/threading/notification.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

static iree_status_t RejectProactorCreation(
    iree_async_proactor_options_t options, iree_allocator_t allocator,
    iree_async_proactor_t** out_proactor) {
  (void)options;
  (void)allocator;
  *out_proactor = nullptr;
  return iree_make_status(IREE_STATUS_ABORTED, "selected creator invoked");
}

struct TestRunnerState;

struct TestRunner {
  TestRunnerState* state = nullptr;
  bool stop_requested = false;
};

struct TestRunnerState {
  static constexpr iree_host_size_t kMaxRunners = 32;

  std::array<TestRunner, kMaxRunners> runners;
  iree_host_size_t create_attempt_count = 0;
  iree_host_size_t create_count = 0;
  iree_host_size_t request_stop_count = 0;
  iree_host_size_t destroy_count = 0;
  iree_host_size_t destroy_without_stop_count = 0;
  iree_host_size_t destroy_before_all_stopped_count = 0;
  iree_host_size_t expected_stop_count_before_destroy = 0;
  iree_host_size_t remaining_create_failures = 0;
};

static iree_status_t TestRunnerCreate(void* user_data,
                                      iree_async_proactor_t* proactor,
                                      uint32_t node_id,
                                      iree_allocator_t allocator,
                                      void** out_runner) {
  (void)proactor;
  (void)node_id;
  (void)allocator;
  TestRunnerState* state = (TestRunnerState*)user_data;
  ++state->create_attempt_count;
  *out_runner = nullptr;
  if (state->remaining_create_failures > 0) {
    --state->remaining_create_failures;
    return iree_make_status(IREE_STATUS_ABORTED,
                            "injected runner creation failure");
  }
  if (state->create_count >= state->runners.size()) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "test runner capacity exceeded");
  }

  TestRunner* runner = &state->runners[state->create_count];
  runner->state = state;
  runner->stop_requested = false;
  ++state->create_count;
  *out_runner = runner;
  return iree_ok_status();
}

static void TestRunnerRequestStop(void* user_data, void* runner_ptr) {
  (void)user_data;
  TestRunner* runner = (TestRunner*)runner_ptr;
  if (!runner->stop_requested) {
    runner->stop_requested = true;
    ++runner->state->request_stop_count;
  }
}

static void TestRunnerDestroy(void* user_data, void* runner_ptr) {
  (void)user_data;
  TestRunner* runner = (TestRunner*)runner_ptr;
  TestRunnerState* state = runner->state;
  if (!runner->stop_requested) {
    ++state->destroy_without_stop_count;
  }
  if (state->request_stop_count < state->expected_stop_count_before_destroy) {
    ++state->destroy_before_all_stopped_count;
  }
  ++state->destroy_count;
}

struct NopCompletionState {
  iree_atomic_int32_t completed = IREE_ATOMIC_VAR_INIT(0);
  iree_notification_t notification;
  iree_status_t status = iree_ok_status();
};

static bool NopCompleted(void* user_data) {
  NopCompletionState* state = (NopCompletionState*)user_data;
  return iree_atomic_load(&state->completed, iree_memory_order_acquire) != 0;
}

static void NopCompletion(void* user_data, iree_async_operation_t* operation,
                          iree_status_t status,
                          iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  NopCompletionState* state = (NopCompletionState*)user_data;
  state->status = status;
  iree_atomic_store(&state->completed, 1, iree_memory_order_release);
  iree_notification_post(&state->notification, IREE_ALL_WAITERS);
}

class ProactorPoolTest : public ::testing::Test {
 protected:
  iree_async_proactor_pool_options_t default_options() {
    return iree_async_proactor_pool_options_default();
  }

  iree_async_proactor_pool_options_t test_runner_options(
      TestRunnerState* state) {
    iree_async_proactor_pool_options_t options = default_options();
    memset(&options.runner, 0, sizeof(options.runner));
    options.runner.user_data = state;
    options.runner.create = TestRunnerCreate;
    options.runner.request_stop = TestRunnerRequestStop;
    options.runner.destroy = TestRunnerDestroy;
    return options;
  }
};

TEST_F(ProactorPoolTest, CreateZeroNodesFails) {
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_async_proactor_pool_create(
                            0, /*node_ids=*/nullptr, default_options(),
                            iree_allocator_system(), &pool));
  EXPECT_EQ(pool, nullptr);
}

TEST_F(ProactorPoolTest, CreateSingleNodeNoAffinity) {
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, default_options(), iree_allocator_system(),
      &pool));
  ASSERT_NE(pool, nullptr);

  EXPECT_EQ(iree_async_proactor_pool_count(pool), 1u);
  EXPECT_EQ(iree_async_proactor_pool_node_id(pool, 0), UINT32_MAX);

  // Out of bounds returns error.
  iree_async_proactor_t* proactor = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_async_proactor_pool_get(pool, 1, &proactor));
  EXPECT_EQ(proactor, nullptr);

  iree_async_proactor_pool_entry_t* entry = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_async_proactor_pool_acquire(pool, 1, &entry));
  EXPECT_EQ(entry, nullptr);
  EXPECT_EQ(iree_async_proactor_pool_node_id(pool, 1), UINT32_MAX);

  iree_async_proactor_pool_release(pool);
}

TEST_F(ProactorPoolTest, CreateAndReleaseWithoutGet) {
  // Pool creation is lazy — creating and releasing without ever calling get
  // should be free (no threads spawned).
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, default_options(), iree_allocator_system(),
      &pool));
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(iree_async_proactor_pool_count(pool), 1u);
  iree_async_proactor_pool_release(pool);
}

TEST_F(ProactorPoolTest, SelectedCreatorIsLazyAndPropagatesFailure) {
  iree_async_proactor_pool_options_t options = default_options();
  options.proactor_create = RejectProactorCreation;
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, options, iree_allocator_system(), &pool));

  iree_async_proactor_t* proactor = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED,
                        iree_async_proactor_pool_get(pool, 0, &proactor));
  EXPECT_EQ(proactor, nullptr);

  iree_async_proactor_pool_entry_t* entry = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED,
                        iree_async_proactor_pool_acquire(pool, 0, &entry));
  EXPECT_EQ(entry, nullptr);

  iree_async_proactor_pool_release(pool);
}

TEST_F(ProactorPoolTest, OnDemandGet) {
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, default_options(), iree_allocator_system(),
      &pool));

  // First get creates the proactor on-demand.
  iree_async_proactor_t* proactor = nullptr;
  iree_status_t status = iree_async_proactor_pool_get(pool, 0, &proactor);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_pool_release(pool);
    GTEST_SKIP() << "Platform proactor unavailable";
  }
  IREE_ASSERT_OK(status);
  EXPECT_NE(proactor, nullptr);

  // Second get returns the same proactor (already created).
  iree_async_proactor_t* proactor_again = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_get(pool, 0, &proactor_again));
  EXPECT_EQ(proactor, proactor_again);

  iree_async_proactor_pool_release(pool);
}

TEST_F(ProactorPoolTest, CreateWithNodeIds) {
  uint32_t node_ids[] = {0, 1};
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      2, node_ids, default_options(), iree_allocator_system(), &pool));
  ASSERT_NE(pool, nullptr);

  EXPECT_EQ(iree_async_proactor_pool_count(pool), 2u);
  EXPECT_EQ(iree_async_proactor_pool_node_id(pool, 0), 0u);
  EXPECT_EQ(iree_async_proactor_pool_node_id(pool, 1), 1u);

  // Each proactor is distinct (on-demand creation).
  iree_async_proactor_t* proactor_0 = nullptr;
  iree_async_proactor_t* proactor_1 = nullptr;
  iree_status_t status = iree_async_proactor_pool_get(pool, 0, &proactor_0);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_pool_release(pool);
    GTEST_SKIP() << "Platform proactor unavailable";
  }
  IREE_ASSERT_OK(status);
  IREE_ASSERT_OK(iree_async_proactor_pool_get(pool, 1, &proactor_1));
  EXPECT_NE(proactor_0, nullptr);
  EXPECT_NE(proactor_1, nullptr);
  EXPECT_NE(proactor_0, proactor_1);

  iree_async_proactor_pool_release(pool);
}

TEST_F(ProactorPoolTest, GetForNodeExactMatch) {
  uint32_t node_ids[] = {3, 7};
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      2, node_ids, default_options(), iree_allocator_system(), &pool));

  // Exact match returns the right proactor.
  iree_async_proactor_t* proactor_3 = nullptr;
  iree_async_proactor_t* proactor_7 = nullptr;
  iree_status_t status =
      iree_async_proactor_pool_get_for_node(pool, 3, &proactor_3);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_pool_release(pool);
    GTEST_SKIP() << "Platform proactor unavailable";
  }
  IREE_ASSERT_OK(status);
  IREE_ASSERT_OK(iree_async_proactor_pool_get_for_node(pool, 7, &proactor_7));

  iree_async_proactor_t* proactor_0 = nullptr;
  iree_async_proactor_t* proactor_1 = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_get(pool, 0, &proactor_0));
  IREE_ASSERT_OK(iree_async_proactor_pool_get(pool, 1, &proactor_1));
  EXPECT_EQ(proactor_3, proactor_0);
  EXPECT_EQ(proactor_7, proactor_1);

  // No match falls back to the first proactor.
  iree_async_proactor_t* proactor_99 = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_get_for_node(pool, 99, &proactor_99));
  EXPECT_EQ(proactor_99, proactor_0);

  // Entry acquisition follows the same exact-match and fallback mapping while
  // retaining runner ownership for the caller.
  iree_async_proactor_pool_entry_t* entry_7 = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_acquire_for_node(pool, 7, &entry_7));
  EXPECT_EQ(iree_async_proactor_pool_entry_node_id(entry_7), 7u);
  EXPECT_EQ(iree_async_proactor_pool_entry_proactor(entry_7), proactor_7);
  iree_async_proactor_pool_entry_release(entry_7);

  iree_async_proactor_pool_entry_t* entry_99 = nullptr;
  IREE_ASSERT_OK(
      iree_async_proactor_pool_acquire_for_node(pool, 99, &entry_99));
  EXPECT_EQ(iree_async_proactor_pool_entry_node_id(entry_99), 3u);
  EXPECT_EQ(iree_async_proactor_pool_entry_proactor(entry_99), proactor_0);
  iree_async_proactor_pool_entry_release(entry_99);

  iree_async_proactor_pool_release(pool);
}

TEST_F(ProactorPoolTest, RetainRelease) {
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, default_options(), iree_allocator_system(),
      &pool));

  // Extra retain keeps the pool alive.
  iree_async_proactor_pool_retain(pool);
  iree_async_proactor_pool_release(pool);  // Drops to ref count 1.

  // Pool should still be usable.
  EXPECT_EQ(iree_async_proactor_pool_count(pool), 1u);

  iree_async_proactor_pool_release(pool);  // Final release, destroys.
}

TEST_F(ProactorPoolTest, AcquiredEntryKeepsRunnerAfterPoolRelease) {
  TestRunnerState runner_state;
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, test_runner_options(&runner_state),
      iree_allocator_system(), &pool));

  // Acquiring the entry owns both the proactor and its progress runner.
  iree_async_proactor_pool_entry_t* entry = nullptr;
  iree_status_t status = iree_async_proactor_pool_acquire(pool, 0, &entry);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_pool_release(pool);
    GTEST_SKIP() << "Platform proactor unavailable";
  }
  IREE_ASSERT_OK(status);
  ASSERT_NE(entry, nullptr);
  iree_async_proactor_t* proactor =
      iree_async_proactor_pool_entry_proactor(entry);
  ASSERT_NE(proactor, nullptr);
  EXPECT_EQ(iree_async_proactor_pool_entry_node_id(entry), UINT32_MAX);
  EXPECT_EQ(runner_state.create_count, 1u);

  // Releasing the aggregate pool must not stop a consumer-retained entry.
  iree_async_proactor_pool_release(pool);
  EXPECT_EQ(runner_state.request_stop_count, 0u);
  EXPECT_EQ(runner_state.destroy_count, 0u);
  EXPECT_EQ(iree_async_proactor_pool_entry_proactor(entry), proactor);

  // The final entry release owns runner stop and destruction.
  runner_state.expected_stop_count_before_destroy = 1;
  iree_async_proactor_pool_entry_release(entry);
  EXPECT_EQ(runner_state.request_stop_count, 1u);
  EXPECT_EQ(runner_state.destroy_count, 1u);
  EXPECT_EQ(runner_state.destroy_without_stop_count, 0u);
  EXPECT_EQ(runner_state.destroy_before_all_stopped_count, 0u);
}

TEST_F(ProactorPoolTest, AcquiredEntryMakesProgressAfterPoolRelease) {
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, default_options(), iree_allocator_system(),
      &pool));

  iree_async_proactor_pool_entry_t* entry = nullptr;
  iree_status_t status = iree_async_proactor_pool_acquire(pool, 0, &entry);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_pool_release(pool);
    GTEST_SKIP() << "Platform proactor unavailable";
  }
  IREE_ASSERT_OK(status);

  iree_async_proactor_t* proactor =
      iree_async_proactor_pool_entry_proactor(entry);
  iree_async_proactor_pool_release(pool);

  NopCompletionState completion;
  iree_notification_initialize(&completion.notification);
  iree_async_nop_operation_t nop;
  iree_async_operation_zero(&nop.base, sizeof(nop));
  iree_async_operation_initialize(&nop.base, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE, NopCompletion,
                                  &completion);

  status = iree_async_proactor_submit_one(proactor, &nop.base);
  if (!iree_status_is_ok(status)) {
    iree_async_proactor_pool_entry_release(entry);
    iree_notification_deinitialize(&completion.notification);
    IREE_ASSERT_OK(status);
  }

  EXPECT_TRUE(iree_notification_await(&completion.notification, NopCompleted,
                                      &completion, iree_infinite_timeout()));

  // Final entry release requests runner stop and joins the polling thread,
  // ensuring the stack operation and callback state are no longer in use.
  iree_async_proactor_pool_entry_release(entry);
  IREE_EXPECT_OK(completion.status);
  iree_notification_deinitialize(&completion.notification);
}

TEST_F(ProactorPoolTest, PoolReleaseStopsAllEntriesBeforeDestroy) {
  constexpr iree_host_size_t kEntryCount = 17;
  TestRunnerState runner_state;
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      kEntryCount, /*node_ids=*/nullptr, test_runner_options(&runner_state),
      iree_allocator_system(), &pool));

  for (iree_host_size_t i = 0; i < kEntryCount; ++i) {
    iree_async_proactor_t* proactor = nullptr;
    iree_status_t status = iree_async_proactor_pool_get(pool, i, &proactor);
    if (iree_status_is_unavailable(status)) {
      iree_status_free(status);
      runner_state.expected_stop_count_before_destroy =
          runner_state.create_count;
      iree_async_proactor_pool_release(pool);
      GTEST_SKIP() << "Platform proactor unavailable";
    }
    IREE_ASSERT_OK(status);
    ASSERT_NE(proactor, nullptr);
  }

  runner_state.expected_stop_count_before_destroy = kEntryCount;
  iree_async_proactor_pool_release(pool);

  EXPECT_EQ(runner_state.create_count, kEntryCount);
  EXPECT_EQ(runner_state.request_stop_count, kEntryCount);
  EXPECT_EQ(runner_state.destroy_count, kEntryCount);
  EXPECT_EQ(runner_state.destroy_without_stop_count, 0u);
  EXPECT_EQ(runner_state.destroy_before_all_stopped_count, 0u);
}

TEST_F(ProactorPoolTest, RunnerCreationFailureCanRetry) {
  TestRunnerState runner_state;
  runner_state.remaining_create_failures = 1;
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, test_runner_options(&runner_state),
      iree_allocator_system(), &pool));

  iree_async_proactor_pool_entry_t* entry = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED,
                        iree_async_proactor_pool_acquire(pool, 0, &entry));
  EXPECT_EQ(entry, nullptr);
  EXPECT_EQ(runner_state.create_attempt_count, 1u);
  EXPECT_EQ(runner_state.create_count, 0u);

  iree_status_t status = iree_async_proactor_pool_acquire(pool, 0, &entry);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_pool_release(pool);
    GTEST_SKIP() << "Platform proactor unavailable";
  }
  IREE_ASSERT_OK(status);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(runner_state.create_attempt_count, 2u);
  EXPECT_EQ(runner_state.create_count, 1u);

  iree_async_proactor_pool_entry_release(entry);
  runner_state.expected_stop_count_before_destroy = 1;
  iree_async_proactor_pool_release(pool);
  EXPECT_EQ(runner_state.request_stop_count, 1u);
  EXPECT_EQ(runner_state.destroy_count, 1u);
}

TEST_F(ProactorPoolTest, ConcurrentAcquireInitializesOneEntry) {
  constexpr iree_host_size_t kThreadCount = 8;
  TestRunnerState runner_state;
  iree_async_proactor_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, /*node_ids=*/nullptr, test_runner_options(&runner_state),
      iree_allocator_system(), &pool));

  std::array<iree_async_proactor_pool_entry_t*, kThreadCount> entries = {};
  std::array<iree_status_t, kThreadCount> statuses = {};
  std::array<std::thread, kThreadCount> threads;
  for (iree_host_size_t i = 0; i < kThreadCount; ++i) {
    threads[i] = std::thread([&, i]() {
      statuses[i] = iree_async_proactor_pool_acquire(pool, 0, &entries[i]);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (iree_host_size_t i = 0; i < kThreadCount; ++i) {
    IREE_EXPECT_OK(statuses[i]);
    ASSERT_NE(entries[i], nullptr);
    EXPECT_EQ(entries[i], entries[0]);
    iree_async_proactor_pool_entry_release(entries[i]);
  }
  EXPECT_EQ(runner_state.create_attempt_count, 1u);
  EXPECT_EQ(runner_state.create_count, 1u);

  runner_state.expected_stop_count_before_destroy = 1;
  iree_async_proactor_pool_release(pool);
  EXPECT_EQ(runner_state.request_stop_count, 1u);
  EXPECT_EQ(runner_state.destroy_count, 1u);
}

}  // namespace
