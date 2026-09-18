// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/util/sequence_emulation.h"

#include "iree/async/util/operation_completion.h"
#include "iree/base/threading/processor.h"

// Acquires the sequence state lock embedded in base.internal_flags. The lock
// only serializes current_step publication with cancellation and is never held
// across a user callback.
static void iree_async_sequence_state_lock(
    iree_async_sequence_operation_t* sequence) {
  int32_t expected = iree_atomic_load(&sequence->base.internal_flags,
                                      iree_memory_order_acquire);
  for (;;) {
    if (expected & IREE_ASYNC_SEQUENCE_INTERNAL_STATE_LOCK) {
      iree_processor_yield();
      expected = iree_atomic_load(&sequence->base.internal_flags,
                                  iree_memory_order_acquire);
      continue;
    }
    int32_t desired = expected | IREE_ASYNC_SEQUENCE_INTERNAL_STATE_LOCK;
    if (iree_atomic_compare_exchange_weak(
            &sequence->base.internal_flags, &expected, desired,
            iree_memory_order_acquire, iree_memory_order_relaxed)) {
      return;
    }
  }
}

static void iree_async_sequence_state_unlock(
    iree_async_sequence_operation_t* sequence) {
  iree_atomic_fetch_and(&sequence->base.internal_flags,
                        (int32_t)~IREE_ASYNC_SEQUENCE_INTERNAL_STATE_LOCK,
                        iree_memory_order_release);
}

static bool iree_async_sequence_is_cancel_requested(
    iree_async_sequence_operation_t* sequence) {
  return iree_any_bit_set(
      iree_async_operation_load_internal_flags(&sequence->base),
      IREE_ASYNC_SEQUENCE_INTERNAL_CANCEL_REQUESTED);
}

// Cancels the stable active step while the sequence state lock is held.
static iree_status_t iree_async_sequence_cancel_active_step_locked(
    iree_async_proactor_t* proactor,
    iree_async_sequence_operation_t* sequence) {
  iree_async_operation_internal_flags_t flags =
      iree_async_operation_load_internal_flags(&sequence->base);
  if (!iree_any_bit_set(flags, IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE)) {
    return iree_ok_status();
  }
  IREE_ASSERT_LT(sequence->current_step, sequence->step_count);
  return iree_async_proactor_cancel(proactor,
                                    sequence->steps[sequence->current_step]);
}

iree_status_t iree_async_sequence_validate(
    const iree_async_sequence_operation_t* sequence) {
  if (iree_any_bit_set(sequence->base.flags,
                       IREE_ASYNC_OPERATION_FLAG_LINKED)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "LINKED flag on SEQUENCE operation is not supported; use the "
        "sequence's steps array to chain operations");
  }
  if (sequence->step_count > 0 && !sequence->steps) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SEQUENCE step array is NULL");
  }
  for (iree_host_size_t i = 0; i < sequence->step_count; ++i) {
    if (!sequence->steps[i]) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "SEQUENCE step %" PRIhsz " is NULL", i);
    }
    if (sequence->steps[i] == &sequence->base) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "SEQUENCE step %" PRIhsz " is self-referential",
                              i);
    }
  }
  return iree_ok_status();
}

void iree_async_sequence_prepare_for_submission(
    iree_async_sequence_operation_t* sequence) {
  iree_async_sequence_state_lock(sequence);
  iree_atomic_store(&sequence->base.internal_flags,
                    IREE_ASYNC_SEQUENCE_INTERNAL_STATE_LOCK,
                    iree_memory_order_relaxed);
  sequence->current_step = 0;
  sequence->internal.proactor = NULL;
  sequence->internal.is_terminal = false;
  sequence->internal.path.stashed_error = NULL;
  iree_async_sequence_state_unlock(sequence);
}

void iree_async_sequence_prepare_for_completion(
    iree_async_sequence_operation_t* sequence) {
  iree_async_sequence_state_lock(sequence);
  sequence->internal.is_terminal = true;
  iree_atomic_fetch_and(&sequence->base.internal_flags,
                        (int32_t)~IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE,
                        iree_memory_order_relaxed);
  iree_async_sequence_state_unlock(sequence);
}

static void iree_async_sequence_complete(
    iree_async_sequence_operation_t* sequence, iree_status_t status) {
  iree_async_sequence_prepare_for_completion(sequence);
  iree_async_operation_complete(&sequence->base, status,
                                IREE_ASYNC_COMPLETION_FLAG_NONE);
}

//===----------------------------------------------------------------------===//
// LINK path (step_fn == NULL)
//===----------------------------------------------------------------------===//

// Completion callback installed on every step during submit_as_linked.
// Coordinates completion counting across all steps and fires the sequence's
// base callback exactly once when all step CQEs have been processed.
//
// The triggering error callback fires before cancelled callbacks for downstream
// steps. The SAW_ERROR flag and internal.path.stashed_error preserve that
// causal error until every step callback has run; otherwise the final cancelled
// step would incorrectly make the whole sequence report CANCELLED.
static void iree_async_sequence_link_trampoline(
    void* user_data, iree_async_operation_t* step, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)step;
  (void)flags;
  iree_async_sequence_operation_t* sequence =
      (iree_async_sequence_operation_t*)user_data;
  const bool step_succeeded = iree_status_is_ok(status);

  // Capture the first non-CANCELLED error. Subsequent errors should not occur
  // in well-formed linked chains, but retain them as diagnostic context if a
  // backend violates that contract.
  bool is_real_error = !iree_status_is_ok(status) &&
                       iree_status_code(status) != IREE_STATUS_CANCELLED;
  if (is_real_error) {
    iree_async_operation_internal_flags_t seq_flags =
        iree_async_operation_load_internal_flags(&sequence->base);
    if (!iree_any_bit_set(seq_flags, IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR)) {
      iree_async_operation_set_internal_flags(
          &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR);
      // Ownership of |status| transfers to the stash.
      sequence->internal.path.stashed_error = status;
      status = iree_ok_status();
    } else {
      sequence->internal.path.stashed_error =
          iree_status_join(sequence->internal.path.stashed_error, status);
      status = iree_ok_status();
    }
  }

  // Handle cancel-step race: if cancel was requested but this step completed
  // with OK (the cancel-step call was a no-op because the step completed
  // between the cancel thread's read of current_step and the cancel
  // submission), record CANCELLED as the final status. Subsequent steps in
  // the kernel chain may still execute, but the sequence result is correct.
  if (iree_status_is_ok(status)) {
    iree_async_operation_internal_flags_t cancel_check_flags =
        iree_async_operation_load_internal_flags(&sequence->base);
    if (iree_any_bit_set(cancel_check_flags,
                         IREE_ASYNC_SEQUENCE_INTERNAL_CANCEL_REQUESTED) &&
        !iree_any_bit_set(cancel_check_flags,
                          IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR)) {
      iree_async_operation_set_internal_flags(
          &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR);
      sequence->internal.path.stashed_error =
          iree_status_from_code(IREE_STATUS_CANCELLED);
    }
  }

  // Publish the next stable active step before cancellation can inspect
  // current_step. A successful linked predecessor has already admitted its
  // successor before this callback. Failed predecessors leave their successors
  // inactive while continuation cancellation invokes their trampolines.
  iree_async_sequence_state_lock(sequence);
  iree_atomic_fetch_and(&sequence->base.internal_flags,
                        (int32_t)~IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE,
                        iree_memory_order_relaxed);
  ++sequence->current_step;
  const bool is_final = sequence->current_step == sequence->step_count;
  if (!is_final && step_succeeded) {
    iree_async_operation_set_internal_flags(
        &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE);

    // Cancellation may have targeted the just-completed predecessor and lost
    // the race. Retarget the now-stable successor before releasing the state
    // lock so an infinite successor cannot be stranded.
    if (iree_async_sequence_is_cancel_requested(sequence)) {
      iree_status_t cancel_status =
          iree_async_sequence_cancel_active_step_locked(
              sequence->internal.proactor, sequence);
      if (iree_status_is_not_found(cancel_status)) {
        // The successor completed before cancellation reached the backend. Its
        // trampoline will publish the following step and retry if needed.
        iree_status_free(cancel_status);
      } else if (!iree_status_is_ok(cancel_status)) {
        iree_async_operation_internal_flags_t sequence_flags =
            iree_async_operation_load_internal_flags(&sequence->base);
        if (!iree_any_bit_set(sequence_flags,
                              IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR)) {
          iree_async_operation_set_internal_flags(
              &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR);
          sequence->internal.path.stashed_error = cancel_status;
        } else {
          sequence->internal.path.stashed_error = iree_status_join(
              sequence->internal.path.stashed_error, cancel_status);
        }
      }
    }
  }
  iree_async_sequence_state_unlock(sequence);

  if (is_final) {
    // All step CQEs processed. Determine final status and fire base callback.
    iree_status_t final_status;
    if (iree_any_bit_set(
            iree_async_operation_load_internal_flags(&sequence->base),
            IREE_ASYNC_SEQUENCE_INTERNAL_SAW_ERROR)) {
      // Use the captured error. The current OK or CANCELLED status has already
      // been accounted for by the aggregate sequence result.
      iree_status_free(status);
      final_status = sequence->internal.path.stashed_error;
      sequence->internal.path.stashed_error = NULL;
    } else {
      // All steps succeeded or the sequence was cancelled. The last step's
      // status carries the right answer: OK if all succeeded, CANCELLED if
      // the sequence (or a predecessor in the linked chain) was cancelled.
      final_status = status;
    }
    iree_async_sequence_complete(sequence, final_status);
  } else {
    // More step completions are pending. The intermediate status has either
    // been stored above or is fully represented by the eventual aggregate.
    iree_status_free(status);
  }
}

iree_status_t iree_async_sequence_submit_as_linked(
    iree_async_proactor_t* proactor,
    iree_async_sequence_operation_t* sequence) {
  // Zero-step edge case: complete without admitting child work. Cancellation
  // that won the state lock is the terminal result.
  if (sequence->step_count == 0) {
    iree_async_sequence_state_lock(sequence);
    bool is_cancelled = iree_async_sequence_is_cancel_requested(sequence);
    iree_async_sequence_state_unlock(sequence);
    iree_async_sequence_complete(
        sequence, is_cancelled ? iree_status_from_code(IREE_STATUS_CANCELLED)
                               : iree_ok_status());
    return iree_ok_status();
  }

  // Save original step state before installing trampolines. On submit failure
  // the steps must be restored so the caller can retry or use them
  // independently.
  typedef struct {
    iree_async_completion_fn_t completion_fn;
    void* user_data;
    iree_async_operation_flags_t flags;
  } iree_async_step_saved_state_t;
  iree_async_step_saved_state_t* saved =
      (iree_async_step_saved_state_t*)iree_alloca(
          sequence->step_count * sizeof(iree_async_step_saved_state_t));

  // Install link trampolines on all steps and set LINKED flags.
  for (iree_host_size_t i = 0; i < sequence->step_count; ++i) {
    iree_async_operation_t* step = sequence->steps[i];
    saved[i].completion_fn = step->completion_fn;
    saved[i].user_data = step->user_data;
    saved[i].flags = step->flags;
    step->completion_fn = iree_async_sequence_link_trampoline;
    step->user_data = sequence;
    if (i + 1 < sequence->step_count) {
      step->flags |= IREE_ASYNC_OPERATION_FLAG_LINKED;
    } else {
      // Last step must NOT have LINKED (contract of linked batches).
      step->flags &= ~IREE_ASYNC_OPERATION_FLAG_LINKED;
    }
  }

  // Serialize child admission with cancellation. A cancellation that wins the
  // lock leaves every child caller-owned; one that follows successful
  // admission observes a stable active step.
  iree_async_sequence_state_lock(sequence);
  if (iree_async_sequence_is_cancel_requested(sequence)) {
    for (iree_host_size_t i = 0; i < sequence->step_count; ++i) {
      iree_async_operation_t* step = sequence->steps[i];
      step->completion_fn = saved[i].completion_fn;
      step->user_data = saved[i].user_data;
      step->flags = saved[i].flags;
    }
    iree_async_sequence_state_unlock(sequence);
    return iree_status_from_code(IREE_STATUS_CANCELLED);
  }

  // Submit the steps as a linked batch through the proactor's vtable. This
  // re-enters the backend submit function, but the expanded steps are not
  // SEQUENCE operations and proceed through normal linked handling.
  sequence->internal.proactor = proactor;
  iree_async_operation_list_t step_list = {sequence->steps,
                                           sequence->step_count};
  iree_status_t status = iree_async_proactor_submit(proactor, step_list);
  if (!iree_status_is_ok(status)) {
    // Submit failed (e.g., SQ full). Restore original step state so the caller
    // can retry or use the steps independently.
    for (iree_host_size_t i = 0; i < sequence->step_count; ++i) {
      iree_async_operation_t* step = sequence->steps[i];
      step->completion_fn = saved[i].completion_fn;
      step->user_data = saved[i].user_data;
      step->flags = saved[i].flags;
      step->linked_next =
          NULL;  // Clear chain links set by the re-entered submit.
    }
    sequence->internal.proactor = NULL;
  } else {
    iree_async_operation_set_internal_flags(
        &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE);
  }
  iree_async_sequence_state_unlock(sequence);
  return status;
}

//===----------------------------------------------------------------------===//
// Emulation path (step_fn != NULL)
//===----------------------------------------------------------------------===//

// Completion callback installed on each step during emulation.
// Recovers the sequence and emulator, then advances to the next step.
static void iree_async_sequence_emulation_trampoline(
    void* user_data, iree_async_operation_t* step, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)step;
  (void)flags;
  iree_async_sequence_operation_t* sequence =
      (iree_async_sequence_operation_t*)user_data;
  iree_async_sequence_emulator_t* emulator =
      (iree_async_sequence_emulator_t*)sequence->internal.path.emulator;
  iree_async_sequence_emulation_step_completed(emulator, sequence, status);
}

// Submits the next step in an emulated sequence.
// Installs the emulation trampoline on the step and submits it.
static iree_status_t iree_async_sequence_emulation_submit_step(
    iree_async_sequence_emulator_t* emulator,
    iree_async_sequence_operation_t* sequence) {
  iree_async_operation_t* step = sequence->steps[sequence->current_step];
  step->completion_fn = iree_async_sequence_emulation_trampoline;
  step->user_data = sequence;
  // Clear LINKED flag on individual steps in emulation mode — each step is
  // submitted independently. The sequence logic handles ordering.
  step->flags &= ~IREE_ASYNC_OPERATION_FLAG_LINKED;
  return emulator->submit_fn(emulator->proactor, step);
}

iree_status_t iree_async_sequence_emulation_begin(
    iree_async_sequence_emulator_t* emulator,
    iree_async_sequence_operation_t* sequence) {
  // Zero-step edge case. Backends normally defer this to their poll owner, but
  // keep direct utility callers well-defined.
  if (sequence->step_count == 0) {
    iree_async_sequence_state_lock(sequence);
    bool is_cancelled = iree_async_sequence_is_cancel_requested(sequence);
    iree_async_sequence_state_unlock(sequence);
    iree_async_sequence_complete(
        sequence, is_cancelled ? iree_status_from_code(IREE_STATUS_CANCELLED)
                               : iree_ok_status());
    return iree_ok_status();
  }

  // Submit step 0. Preserve caller state until admission succeeds so a
  // synchronous failure leaves both the sequence and its first step reusable.
  iree_async_operation_t* first_step = sequence->steps[0];
  iree_async_completion_fn_t saved_completion_fn = first_step->completion_fn;
  void* saved_user_data = first_step->user_data;
  iree_async_operation_flags_t saved_flags = first_step->flags;

  // Serialize first-step admission with cancellation. A cancellation accepted
  // before startup completes the sequence without touching caller-owned child
  // state. Once submission succeeds, cancellation observes a stable active
  // child before the lock is released.
  iree_async_sequence_state_lock(sequence);
  if (iree_async_sequence_is_cancel_requested(sequence)) {
    iree_async_sequence_state_unlock(sequence);
    return iree_status_from_code(IREE_STATUS_CANCELLED);
  }
  sequence->internal.proactor = emulator->proactor;
  sequence->internal.path.emulator = emulator;
  iree_status_t status =
      iree_async_sequence_emulation_submit_step(emulator, sequence);
  if (!iree_status_is_ok(status)) {
    first_step->completion_fn = saved_completion_fn;
    first_step->user_data = saved_user_data;
    first_step->flags = saved_flags;
    sequence->internal.proactor = NULL;
    sequence->internal.path.emulator = NULL;
  } else {
    iree_async_operation_set_internal_flags(
        &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE);
  }
  iree_async_sequence_state_unlock(sequence);
  return status;
}

void iree_async_sequence_emulation_step_completed(
    iree_async_sequence_emulator_t* emulator,
    iree_async_sequence_operation_t* sequence, iree_status_t step_status) {
  // Retire the stable active child before inspecting its result. Cancellation
  // may have held the state lock while submitting a backend cancel request;
  // waiting here keeps current_step and child ownership stable for that call.
  iree_async_sequence_state_lock(sequence);
  iree_atomic_fetch_and(&sequence->base.internal_flags,
                        (int32_t)~IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE,
                        iree_memory_order_relaxed);
  ++sequence->current_step;
  iree_async_sequence_state_unlock(sequence);

  // Step failure: abort immediately.
  if (!iree_status_is_ok(step_status)) {
    iree_async_sequence_complete(sequence, step_status);
    return;
  }

  // Check if cancel was requested between steps.
  if (iree_async_sequence_is_cancel_requested(sequence)) {
    iree_async_sequence_complete(sequence,
                                 iree_status_from_code(IREE_STATUS_CANCELLED));
    return;
  }

  // Determine the completed step and the next step (NULL if this was the last).
  iree_async_operation_t* completed_step =
      sequence->steps[sequence->current_step - 1];
  iree_async_operation_t* next_step =
      (sequence->current_step < sequence->step_count)
          ? sequence->steps[sequence->current_step]
          : NULL;

  // Call the inter-step callback if provided.
  if (sequence->step_fn) {
    iree_status_t step_fn_status =
        sequence->step_fn(sequence->base.user_data, completed_step, next_step);
    if (!iree_status_is_ok(step_fn_status)) {
      // step_fn vetoed continuation. Abort with its error.
      iree_async_sequence_complete(sequence, step_fn_status);
      return;
    }
  }

  // All steps complete?
  if (sequence->current_step == sequence->step_count) {
    iree_async_sequence_complete(sequence, iree_ok_status());
    return;
  }

  // Re-check cancel after step_fn. Cancel may have arrived during step_fn
  // execution (which can take arbitrary time). Without this check, the next
  // step would be submitted despite the cancel request.
  if (iree_async_sequence_is_cancel_requested(sequence)) {
    iree_async_sequence_complete(sequence,
                                 iree_status_from_code(IREE_STATUS_CANCELLED));
    return;
  }

  // Serialize next-step admission with cancellation. A cancellation that wins
  // the lock leaves the next child unsubmitted. A cancellation that follows a
  // successful submission observes the new child as active.
  iree_async_sequence_state_lock(sequence);
  if (iree_async_sequence_is_cancel_requested(sequence)) {
    iree_async_sequence_state_unlock(sequence);
    iree_async_sequence_complete(sequence,
                                 iree_status_from_code(IREE_STATUS_CANCELLED));
    return;
  }
  iree_status_t submit_status =
      iree_async_sequence_emulation_submit_step(emulator, sequence);
  if (iree_status_is_ok(submit_status)) {
    iree_async_operation_set_internal_flags(
        &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_STEP_ACTIVE);
  }
  iree_async_sequence_state_unlock(sequence);
  if (!iree_status_is_ok(submit_status)) {
    // Next step submission failed. Abort with the submission error.
    iree_async_sequence_complete(sequence, submit_status);
  }
}

//===----------------------------------------------------------------------===//
// Cancellation
//===----------------------------------------------------------------------===//

iree_status_t iree_async_sequence_cancel(
    iree_async_proactor_t* proactor,
    iree_async_sequence_operation_t* sequence) {
  iree_async_sequence_state_lock(sequence);
  if (sequence->internal.is_terminal) {
    iree_async_sequence_state_unlock(sequence);
    return iree_ok_status();
  }
  iree_async_operation_set_internal_flags(
      &sequence->base, IREE_ASYNC_SEQUENCE_INTERNAL_CANCEL_REQUESTED);

  // Cancel the stable active child, if any. Pre-start and inter-step
  // cancellation has no child to target; startup/progression observes the
  // request while holding the same lock and terminates the sequence instead.
  iree_status_t status =
      iree_async_sequence_cancel_active_step_locked(proactor, sequence);
  iree_async_sequence_state_unlock(sequence);

  if (iree_status_is_not_found(status)) {
    // The child completed in the backend but its poll callback has not advanced
    // the sequence yet. The recorded request makes that callback cancel or
    // suppress its successor, so cancellation is accepted at sequence scope.
    iree_status_free(status);
    return iree_ok_status();
  }
  return status;
}
