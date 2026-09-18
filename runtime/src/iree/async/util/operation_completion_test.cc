// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/util/operation_completion.h"

#include <vector>

#include "iree/async/operation.h"
#include "iree/async/operations/file.h"
#include "iree/async/operations/net.h"
#include "iree/async/region.h"
#include "iree/async/util/operation_pool.h"
#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct CompletionRecord {
  iree_host_size_t call_count = 0;
  iree_status_code_t status_code = IREE_STATUS_OK;
  iree_async_completion_flags_t flags = IREE_ASYNC_COMPLETION_FLAG_NONE;
  std::vector<iree_async_operation_internal_flags_t> internal_flags;
  iree_async_operation_pool_t* pool = nullptr;
  iree_async_operation_t* acquired_during_callback = nullptr;
};

struct TestRegion {
  iree_async_region_t base;
  bool destroyed = false;
};

static void DestroyTestRegion(iree_async_region_t* base_region) {
  TestRegion* region = reinterpret_cast<TestRegion*>(base_region);
  region->destroyed = true;
}

static void InitializeTestRegion(TestRegion* region) {
  iree_atomic_ref_count_init(&region->base.ref_count);
  region->base.destroy_fn = DestroyTestRegion;
}

struct RegionCompletionRecord {
  TestRegion* expected_region = nullptr;
  int32_t expected_ref_count = 0;
  TestRegion* resubmit_region = nullptr;
  iree_host_size_t call_count = 0;
};

static void RecordRegionCompletion(void* user_data,
                                   iree_async_operation_t* operation,
                                   iree_status_t status,
                                   iree_async_completion_flags_t flags) {
  (void)flags;
  RegionCompletionRecord* record =
      static_cast<RegionCompletionRecord*>(user_data);
  ++record->call_count;
  EXPECT_FALSE(record->expected_region->destroyed);
  EXPECT_EQ(
      iree_atomic_ref_count_load(&record->expected_region->base.ref_count),
      record->expected_ref_count);
  iree_status_free(status);

  if (record->resubmit_region) {
    iree_async_file_read_operation_t* read_op =
        reinterpret_cast<iree_async_file_read_operation_t*>(operation);
    read_op->buffer.region = &record->resubmit_region->base;
    iree_async_operation_acquire_resources(operation);
    record->expected_region = record->resubmit_region;
    record->expected_ref_count = 1;
    record->resubmit_region = nullptr;
  }
}

static void RecordCompletion(void* user_data, iree_async_operation_t* operation,
                             iree_status_t status,
                             iree_async_completion_flags_t flags) {
  (void)operation;
  CompletionRecord* record = static_cast<CompletionRecord*>(user_data);
  ++record->call_count;
  record->status_code = iree_status_code(status);
  record->flags = flags;
  record->internal_flags.push_back(
      iree_async_operation_load_internal_flags(operation));
  iree_status_free(status);
  if (record->pool) {
    IREE_ASSERT_OK(iree_async_operation_pool_acquire(
        record->pool, sizeof(*operation), &record->acquired_during_callback));
  }
}

TEST(OperationCompletionTest, TransfersStatusToCallback) {
  CompletionRecord record;
  iree_async_operation_t operation;
  iree_async_operation_initialize(&operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  RecordCompletion, &record);

  EXPECT_EQ(iree_async_operation_complete(
                &operation,
                iree_make_status(IREE_STATUS_DATA_LOSS, "completion failure"),
                IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_EQ(record.call_count, 1u);
  EXPECT_EQ(record.status_code, IREE_STATUS_DATA_LOSS);
}

TEST(OperationCompletionTest, FreesSuppressedStatusWithoutCallback) {
  iree_async_operation_t operation;
  iree_async_operation_initialize(&operation, IREE_ASYNC_OPERATION_TYPE_MESSAGE,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  /*completion_fn=*/nullptr,
                                  /*user_data=*/nullptr);

  EXPECT_EQ(iree_async_operation_complete(
                &operation,
                iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED, "pool full"),
                IREE_ASYNC_COMPLETION_FLAG_NONE),
            0u);
}

TEST(OperationCompletionTest, ResolvesCancellationAsSuccess) {
  CompletionRecord record;
  iree_async_operation_t operation;
  iree_async_operation_initialize(
      &operation, IREE_ASYNC_OPERATION_TYPE_NOP,
      IREE_ASYNC_OPERATION_FLAG_CANCELLATION_IS_SUCCESS, RecordCompletion,
      &record);

  EXPECT_EQ(iree_async_operation_complete(
                &operation, iree_status_from_code(IREE_STATUS_CANCELLED),
                IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_EQ(record.status_code, IREE_STATUS_OK);
  EXPECT_TRUE(
      iree_any_bit_set(record.flags, IREE_ASYNC_COMPLETION_FLAG_CANCELLED));
}

TEST(OperationCompletionTest, ClearsPrivateStateAtFinalOwnershipHandoff) {
  CompletionRecord record;
  iree_async_operation_t operation;
  iree_async_operation_initialize(&operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  RecordCompletion, &record);
  iree_async_operation_set_internal_flags(&operation, 0x5A);

  EXPECT_EQ(iree_async_operation_complete(&operation, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_MORE),
            1u);
  EXPECT_EQ(iree_async_operation_load_internal_flags(&operation), 0x5Au);

  EXPECT_EQ(iree_async_operation_complete(&operation, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  ASSERT_EQ(record.internal_flags.size(), 2u);
  EXPECT_EQ(record.internal_flags[0], 0x5Au);
  EXPECT_EQ(record.internal_flags[1], 0u);
  EXPECT_EQ(iree_async_operation_load_internal_flags(&operation), 0u);
}

TEST(OperationCompletionTest, ReturnsFinalOperationToPoolAfterCallback) {
  iree_async_operation_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_allocate(
      iree_async_operation_pool_options_default(), iree_allocator_system(),
      &pool));

  iree_async_operation_t* operation = nullptr;
  IREE_ASSERT_OK(
      iree_async_operation_pool_acquire(pool, sizeof(*operation), &operation));
  CompletionRecord record;
  record.pool = pool;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  RecordCompletion, &record);
  operation->pool = pool;

  EXPECT_EQ(iree_async_operation_complete(operation, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_NE(record.acquired_during_callback, operation);

  iree_async_operation_t* recycled_operation = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_acquire(
      pool, sizeof(*recycled_operation), &recycled_operation));
  EXPECT_EQ(recycled_operation, operation);
  iree_async_operation_pool_release(pool, recycled_operation);
  iree_async_operation_pool_release(pool, record.acquired_during_callback);
  iree_async_operation_pool_free(pool);
}

TEST(OperationCompletionTest, RetainsPooledMultishotUntilFinalCompletion) {
  iree_async_operation_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_allocate(
      iree_async_operation_pool_options_default(), iree_allocator_system(),
      &pool));

  iree_async_operation_t* operation = nullptr;
  IREE_ASSERT_OK(
      iree_async_operation_pool_acquire(pool, sizeof(*operation), &operation));
  CompletionRecord record;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  RecordCompletion, &record);
  operation->pool = pool;

  EXPECT_EQ(iree_async_operation_complete(operation, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_MORE),
            1u);

  iree_async_operation_t* acquired_operation = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_acquire(
      pool, sizeof(*acquired_operation), &acquired_operation));
  EXPECT_NE(acquired_operation, operation);
  iree_async_operation_pool_release(pool, acquired_operation);

  EXPECT_EQ(iree_async_operation_complete(operation, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  IREE_ASSERT_OK(iree_async_operation_pool_acquire(
      pool, sizeof(*acquired_operation), &acquired_operation));
  EXPECT_EQ(acquired_operation, operation);
  iree_async_operation_pool_release(pool, acquired_operation);
  iree_async_operation_pool_free(pool);
}

TEST(OperationCompletionTest, RetainsRegionThroughFinalCallback) {
  TestRegion region = {};
  InitializeTestRegion(&region);

  RegionCompletionRecord record;
  record.expected_region = &region;
  record.expected_ref_count = 1;
  iree_async_file_read_operation_t read_op = {};
  iree_async_operation_initialize(
      &read_op.base, IREE_ASYNC_OPERATION_TYPE_FILE_READ,
      IREE_ASYNC_OPERATION_FLAG_MULTISHOT, RecordRegionCompletion, &record);
  read_op.buffer = iree_async_span_make(&region.base, 0, 1);

  iree_async_operation_acquire_resources(&read_op.base);
  EXPECT_EQ(iree_atomic_ref_count_load(&region.base.ref_count), 2);
  iree_async_region_release(&region.base);

  EXPECT_EQ(iree_async_operation_complete(&read_op.base, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_MORE),
            1u);
  EXPECT_FALSE(region.destroyed);
  EXPECT_EQ(read_op.base.acquired_span_count, 1);

  EXPECT_EQ(iree_async_operation_complete(&read_op.base, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_TRUE(region.destroyed);
  EXPECT_EQ(read_op.base.acquired_span_count, 0);
  EXPECT_EQ(record.call_count, 2u);
}

TEST(OperationCompletionTest, ResubmitDoesNotAliasPriorRegionOwnership) {
  TestRegion first_region = {};
  TestRegion second_region = {};
  InitializeTestRegion(&first_region);
  InitializeTestRegion(&second_region);

  RegionCompletionRecord record;
  record.expected_region = &first_region;
  record.expected_ref_count = 1;
  record.resubmit_region = &second_region;
  iree_async_file_read_operation_t read_op = {};
  iree_async_operation_initialize(
      &read_op.base, IREE_ASYNC_OPERATION_TYPE_FILE_READ,
      IREE_ASYNC_OPERATION_FLAG_NONE, RecordRegionCompletion, &record);
  read_op.buffer = iree_async_span_make(&first_region.base, 0, 1);

  iree_async_operation_acquire_resources(&read_op.base);
  iree_async_region_release(&first_region.base);
  EXPECT_EQ(iree_async_operation_complete(&read_op.base, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_TRUE(first_region.destroyed);
  EXPECT_FALSE(second_region.destroyed);
  EXPECT_EQ(iree_atomic_ref_count_load(&second_region.base.ref_count), 2);

  iree_async_region_release(&second_region.base);
  EXPECT_EQ(iree_async_operation_complete(&read_op.base, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_TRUE(second_region.destroyed);
  EXPECT_EQ(record.call_count, 2u);
}

TEST(OperationCompletionTest, RetainsDuplicateSpanRegionsIndependently) {
  TestRegion region = {};
  InitializeTestRegion(&region);

  RegionCompletionRecord record;
  record.expected_region = &region;
  record.expected_ref_count = 2;
  iree_async_socket_send_operation_t send_op = {};
  iree_async_operation_initialize(
      &send_op.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_SEND,
      IREE_ASYNC_OPERATION_FLAG_NONE, RecordRegionCompletion, &record);
  iree_async_span_t spans[] = {
      iree_async_span_make(&region.base, 0, 1),
      iree_async_span_from_ptr(reinterpret_cast<void*>(uintptr_t{1}), 1),
      iree_async_span_make(&region.base, 1, 1),
  };
  send_op.buffers = iree_async_span_list_make(spans, IREE_ARRAYSIZE(spans));

  iree_async_operation_acquire_resources(&send_op.base);
  EXPECT_EQ(iree_atomic_ref_count_load(&region.base.ref_count), 3);
  EXPECT_EQ(send_op.base.acquired_span_count, 3);
  EXPECT_EQ(send_op.retained_buffer_regions[0], &region.base);
  EXPECT_EQ(send_op.retained_buffer_regions[1], nullptr);
  EXPECT_EQ(send_op.retained_buffer_regions[2], &region.base);
  iree_async_region_release(&region.base);
  EXPECT_EQ(iree_async_operation_complete(&send_op.base, iree_ok_status(),
                                          IREE_ASYNC_COMPLETION_FLAG_NONE),
            1u);
  EXPECT_TRUE(region.destroyed);
  EXPECT_EQ(send_op.base.acquired_span_count, 0);
}

}  // namespace
