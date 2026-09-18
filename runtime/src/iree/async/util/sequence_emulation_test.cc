// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/util/sequence_emulation.h"

#include "iree/async/util/operation_pool.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct CompletionState {
  int call_count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;
};

void TrackCompletion(void* user_data, iree_async_operation_t* operation,
                     iree_status_t status,
                     iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  auto* state = static_cast<CompletionState*>(user_data);
  ++state->call_count;
  state->status_code = iree_status_code(status);
  iree_status_free(status);
}

void OriginalStepCompletion(void* user_data, iree_async_operation_t* operation,
                            iree_status_t status,
                            iree_async_completion_flags_t flags) {
  (void)user_data;
  (void)operation;
  (void)flags;
  iree_status_free(status);
}

iree_status_t RejectStepSubmission(iree_async_proactor_t* proactor,
                                   iree_async_operation_t* operation) {
  (void)proactor;
  (void)operation;
  return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                          "injected admission failure");
}

iree_status_t ContinueSequence(void* user_data,
                               iree_async_operation_t* completed_step,
                               iree_async_operation_t* next_step) {
  (void)user_data;
  (void)completed_step;
  (void)next_step;
  return iree_ok_status();
}

TEST(SequenceEmulationTest, RejectsMalformedSequenceShape) {
  iree_async_sequence_operation_t sequence;
  iree_async_operation_zero(&sequence.base, sizeof(sequence));
  iree_async_operation_initialize(
      &sequence.base, IREE_ASYNC_OPERATION_TYPE_SEQUENCE,
      IREE_ASYNC_OPERATION_FLAG_NONE, /*completion_fn=*/nullptr,
      /*user_data=*/nullptr);

  sequence.step_count = 1;
  sequence.steps = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_async_sequence_validate(&sequence));

  iree_async_operation_t* steps[] = {nullptr};
  sequence.steps = steps;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_async_sequence_validate(&sequence));

  steps[0] = &sequence.base;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_async_sequence_validate(&sequence));

  sequence.step_count = 0;
  sequence.base.flags = IREE_ASYNC_OPERATION_FLAG_LINKED;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_async_sequence_validate(&sequence));
}

TEST(SequenceEmulationTest, InitialSubmitFailurePreservesCallerOwnership) {
  int step_user_data = 0;
  iree_async_nop_operation_t step;
  iree_async_operation_zero(&step.base, sizeof(step));
  iree_async_operation_initialize(
      &step.base, IREE_ASYNC_OPERATION_TYPE_NOP,
      IREE_ASYNC_OPERATION_FLAG_LINKED |
          IREE_ASYNC_OPERATION_FLAG_CANCELLATION_IS_SUCCESS,
      OriginalStepCompletion, &step_user_data);

  CompletionState completion_state;
  iree_async_operation_t* steps[] = {&step.base};
  iree_async_sequence_operation_t sequence;
  iree_async_operation_zero(&sequence.base, sizeof(sequence));
  iree_async_operation_initialize(
      &sequence.base, IREE_ASYNC_OPERATION_TYPE_SEQUENCE,
      IREE_ASYNC_OPERATION_FLAG_NONE, TrackCompletion, &completion_state);
  sequence.steps = steps;
  sequence.step_count = IREE_ARRAYSIZE(steps);
  sequence.step_fn = ContinueSequence;

  iree_async_sequence_emulator_t emulator;
  iree_async_sequence_emulator_initialize(&emulator, /*proactor=*/nullptr,
                                          RejectStepSubmission);
  iree_async_sequence_prepare_for_submission(&sequence);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_async_sequence_emulation_begin(&emulator, &sequence));

  EXPECT_EQ(completion_state.call_count, 0);
  EXPECT_EQ(step.base.completion_fn, &OriginalStepCompletion);
  EXPECT_EQ(step.base.user_data, &step_user_data);
  EXPECT_EQ(step.base.flags,
            IREE_ASYNC_OPERATION_FLAG_LINKED |
                IREE_ASYNC_OPERATION_FLAG_CANCELLATION_IS_SUCCESS);
  EXPECT_EQ(sequence.internal.path.emulator, nullptr);
}

TEST(SequenceEmulationTest, CancellationBeforeStartupDoesNotSubmitChild) {
  int step_user_data = 0;
  iree_async_nop_operation_t step;
  iree_async_operation_zero(&step.base, sizeof(step));
  iree_async_operation_initialize(&step.base, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_LINKED,
                                  OriginalStepCompletion, &step_user_data);

  CompletionState completion_state;
  iree_async_operation_t* steps[] = {&step.base};
  iree_async_sequence_operation_t sequence;
  iree_async_operation_zero(&sequence.base, sizeof(sequence));
  iree_async_operation_initialize(
      &sequence.base, IREE_ASYNC_OPERATION_TYPE_SEQUENCE,
      IREE_ASYNC_OPERATION_FLAG_NONE, TrackCompletion, &completion_state);
  sequence.steps = steps;
  sequence.step_count = IREE_ARRAYSIZE(steps);
  sequence.step_fn = ContinueSequence;

  iree_async_sequence_emulator_t emulator;
  iree_async_sequence_emulator_initialize(&emulator, /*proactor=*/nullptr,
                                          RejectStepSubmission);
  iree_async_sequence_prepare_for_submission(&sequence);
  IREE_ASSERT_OK(iree_async_sequence_cancel(/*proactor=*/nullptr, &sequence));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_CANCELLED,
      iree_async_sequence_emulation_begin(&emulator, &sequence));

  EXPECT_EQ(completion_state.call_count, 0);
  EXPECT_EQ(step.base.completion_fn, &OriginalStepCompletion);
  EXPECT_EQ(step.base.user_data, &step_user_data);
  EXPECT_EQ(step.base.flags, IREE_ASYNC_OPERATION_FLAG_LINKED);
  EXPECT_EQ(sequence.current_step, 0u);
  EXPECT_EQ(sequence.internal.path.emulator, nullptr);
}

TEST(SequenceEmulationTest, TerminalPreparationJoinsCancellation) {
  iree_async_nop_operation_t step;
  iree_async_operation_zero(&step.base, sizeof(step));
  iree_async_operation_initialize(&step.base, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  OriginalStepCompletion,
                                  /*user_data=*/nullptr);

  CompletionState completion_state;
  iree_async_operation_t* steps[] = {&step.base};
  iree_async_sequence_operation_t sequence;
  iree_async_operation_zero(&sequence.base, sizeof(sequence));
  iree_async_operation_initialize(
      &sequence.base, IREE_ASYNC_OPERATION_TYPE_SEQUENCE,
      IREE_ASYNC_OPERATION_FLAG_NONE, TrackCompletion, &completion_state);
  sequence.steps = steps;
  sequence.step_count = IREE_ARRAYSIZE(steps);

  iree_async_sequence_prepare_for_submission(&sequence);
  iree_async_sequence_prepare_for_completion(&sequence);
  IREE_ASSERT_OK(iree_async_sequence_cancel(/*proactor=*/nullptr, &sequence));

  EXPECT_TRUE(sequence.internal.is_terminal);
  EXPECT_FALSE(
      iree_any_bit_set(iree_async_operation_load_internal_flags(&sequence.base),
                       IREE_ASYNC_SEQUENCE_INTERNAL_CANCEL_REQUESTED));
  EXPECT_EQ(step.base.completion_fn, &OriginalStepCompletion);
}

TEST(SequenceEmulationTest, TerminalCompletionReturnsOperationToPool) {
  iree_async_operation_pool_t* pool = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_allocate(
      iree_async_operation_pool_options_default(), iree_allocator_system(),
      &pool));

  iree_async_operation_t* base_operation = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_acquire(
      pool, sizeof(iree_async_sequence_operation_t), &base_operation));
  auto* sequence =
      reinterpret_cast<iree_async_sequence_operation_t*>(base_operation);
  CompletionState completion_state;
  iree_async_operation_initialize(
      &sequence->base, IREE_ASYNC_OPERATION_TYPE_SEQUENCE,
      IREE_ASYNC_OPERATION_FLAG_NONE, TrackCompletion, &completion_state);
  sequence->base.pool = pool;
  sequence->step_count = 0;

  iree_async_operation_t* released_operation = &sequence->base;
  iree_async_sequence_prepare_for_submission(sequence);
  IREE_ASSERT_OK(
      iree_async_sequence_submit_as_linked(/*proactor=*/nullptr, sequence));
  EXPECT_EQ(completion_state.call_count, 1);
  EXPECT_EQ(completion_state.status_code, IREE_STATUS_OK);

  iree_async_operation_t* reacquired_operation = nullptr;
  IREE_ASSERT_OK(iree_async_operation_pool_acquire(
      pool, sizeof(iree_async_sequence_operation_t), &reacquired_operation));
  EXPECT_EQ(reacquired_operation, released_operation);
  iree_async_operation_pool_release(pool, reacquired_operation);
  iree_async_operation_pool_free(pool);
}

}  // namespace
