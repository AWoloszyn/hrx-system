// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <functional>
#include <utility>

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/test_base.h"
#include "iree/async/operations/scheduling.h"

namespace iree::async::cts {
namespace {

struct ProgressWork {
  explicit ProgressWork(
      std::function<iree_status_t(iree_host_size_t*)> callback = {})
      : callback(std::move(callback)) {
    entry.user_data = this;
    entry.fn = +[](void* user_data, iree_host_size_t* out_completed_count) {
      return static_cast<ProgressWork*>(user_data)->callback(
          out_completed_count);
    };
  }

  // Caller-owned registration kept alive until explicit removal.
  iree_async_progress_entry_t entry = {};
  // Test owner work invoked by the real proactor poll loop.
  std::function<iree_status_t(iree_host_size_t*)> callback;
};

class ProgressTest : public CtsTestBase<> {
 protected:
  void Register(ProgressWork& work) {
    iree_async_proactor_register_progress(proactor_, &work.entry);
  }
};

TEST_P(ProgressTest, FailureRetainsEntryUntilExplicitRetirement) {
  int calls = 0;
  ProgressWork work([&](iree_host_size_t* completed) {
    EXPECT_EQ(*completed, 0u);
    ++calls;
    *completed = 1;
    if (calls == 1) {
      return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
    }
    work.entry.remove_requested = true;
    return iree_ok_status();
  });
  Register(work);

  iree_host_size_t completed = 99;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(proactor_->progress_list, &work.entry);

  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, FailurePreservesEarlierCountsAndUnvisitedWork) {
  ProgressWork work[3];
  int calls[3] = {};
  int total_calls = 0;
  iree_host_size_t dispatched = 0;
  for (int i = 0; i < 3; ++i) {
    work[i].callback = [&, i](iree_host_size_t* completed) {
      ++calls[i];
      ++total_calls;
      *completed = i + 1;
      dispatched += *completed;
      if (total_calls == 2) {
        return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
      }
      work[i].entry.remove_requested = true;
      return iree_ok_status();
    };
    Register(work[i]);
  }

  iree_host_size_t completed = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, dispatched);
  EXPECT_EQ(total_calls, 2);

  dispatched = 0;
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, dispatched);
  EXPECT_EQ(total_calls, 4);
  for (int count : calls) {
    EXPECT_GE(count, 1);
    EXPECT_LE(count, 2);
  }
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, FailureStillUnlinksBeforeCallbackOwnedDestruction) {
  struct Owner {
    // Embedded registration destroyed only after the proactor unlinks it.
    iree_async_progress_entry_t entry = {};
    // External witness that survives the owner.
    bool* destroyed = nullptr;
  };
  bool destroyed = false;
  auto* owner = new Owner;
  owner->destroyed = &destroyed;
  owner->entry.user_data = owner;
  owner->entry.fn =
      +[](void* user_data, iree_host_size_t* completed) -> iree_status_t {
    auto* owner = static_cast<Owner*>(user_data);
    *completed = 1;
    owner->entry.remove_requested = true;
    return iree_make_status(IREE_STATUS_UNAVAILABLE, "owner progress failed");
  };
  owner->entry.on_remove = +[](void* user_data) {
    auto* owner = static_cast<Owner*>(user_data);
    EXPECT_EQ(owner->entry.next, nullptr);
    EXPECT_FALSE(owner->entry.remove_requested);
    *owner->destroyed = true;
    delete owner;
  };
  iree_async_proactor_register_progress(proactor_, &owner->entry);

  iree_host_size_t completed = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_TRUE(destroyed);
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, WorkRegisteredDuringCallbackRunsOnNextPoll) {
  int successor_calls = 0;
  ProgressWork successor([&](iree_host_size_t* completed) {
    ++successor_calls;
    *completed = 1;
    successor.entry.remove_requested = true;
    return iree_ok_status();
  });
  ProgressWork first([&](iree_host_size_t* completed) {
    Register(successor);
    *completed = 1;
    first.entry.remove_requested = true;
    return iree_ok_status();
  });
  Register(first);

  iree_host_size_t completed = 0;
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(successor_calls, 0);
  IREE_EXPECT_OK(iree_async_proactor_poll(proactor_, iree_immediate_timeout(),
                                          &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(successor_calls, 1);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, FailedCallbackPreservesItsNewRegistration) {
  int successor_calls = 0;
  ProgressWork successor([&](iree_host_size_t* completed) {
    ++successor_calls;
    *completed = 1;
    successor.entry.remove_requested = true;
    return iree_ok_status();
  });
  ProgressWork first([&](iree_host_size_t*) {
    Register(successor);
    first.entry.remove_requested = true;
    return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
  });
  Register(first);

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), nullptr));
  EXPECT_EQ(successor_calls, 0);
  iree_host_size_t completed = 0;
  IREE_EXPECT_OK(iree_async_proactor_poll(proactor_, iree_immediate_timeout(),
                                          &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(successor_calls, 1);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, RemovalCallbackCanRearmForNextPoll) {
  struct Owner {
    // Proactor owning the registration.
    iree_async_proactor_t* proactor = nullptr;
    // Reused only after on_remove observes the entry unlinked.
    iree_async_progress_entry_t entry = {};
    // Number of independent progress turns observed.
    int calls = 0;
  } owner;
  owner.proactor = proactor_;
  owner.entry.user_data = &owner;
  owner.entry.fn = +[](void* user_data, iree_host_size_t* completed) {
    auto* owner = static_cast<Owner*>(user_data);
    ++owner->calls;
    *completed = 1;
    owner->entry.remove_requested = true;
    return iree_ok_status();
  };
  owner.entry.on_remove = +[](void* user_data) {
    auto* owner = static_cast<Owner*>(user_data);
    EXPECT_EQ(owner->entry.next, nullptr);
    if (owner->calls == 1) {
      iree_async_proactor_register_progress(owner->proactor, &owner->entry);
    }
  };
  iree_async_proactor_register_progress(proactor_, &owner.entry);

  iree_host_size_t completed = 0;
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(owner.calls, 1);
  IREE_EXPECT_OK(iree_async_proactor_poll(proactor_, iree_immediate_timeout(),
                                          &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(owner.calls, 2);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, CallbackCanDestroyUnvisitedEntry) {
  ProgressWork* work[2];
  int calls = 0;
  for (int i = 0; i < 2; ++i) {
    work[i] = new ProgressWork([&, i](iree_host_size_t* completed) {
      ++calls;
      iree_async_proactor_unregister_progress(proactor_, &work[1 - i]->entry);
      delete work[1 - i];
      *completed = 1;
      work[i]->entry.remove_requested = true;
      return iree_ok_status();
    });
    work[i]->entry.on_remove =
        +[](void* user_data) { delete static_cast<ProgressWork*>(user_data); };
    Register(*work[i]);
  }

  iree_host_size_t completed = 0;
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, CallbackCanDestroyPreviouslyRunEntry) {
  ProgressWork* work[2];
  ProgressWork* earlier = nullptr;
  int calls = 0;
  for (int i = 0; i < 2; ++i) {
    work[i] = new ProgressWork([&, i](iree_host_size_t* completed) {
      ++calls;
      *completed = 1;
      if (!earlier) {
        earlier = work[i];
      } else {
        iree_async_proactor_unregister_progress(proactor_, &earlier->entry);
        delete earlier;
        work[i]->entry.remove_requested = true;
      }
      return iree_ok_status();
    });
    work[i]->entry.on_remove =
        +[](void* user_data) { delete static_cast<ProgressWork*>(user_data); };
    Register(*work[i]);
  }

  iree_host_size_t completed = 0;
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 2u);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, IdleRegisteredWorkPreventsBlocking) {
  int calls = 0;
  ProgressWork work([&](iree_host_size_t* completed) {
    ++calls;
    if (calls == 2) {
      *completed = 1;
      work.entry.remove_requested = true;
    }
    return iree_ok_status();
  });
  Register(work);

  // No timer or native completion can release a backend that wrongly blocks.
  // The outer test harness catches the hang; no local deadline masks it.
  iree_host_size_t completed = 99;
  iree_status_t status =
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed);
  if (iree_status_is_deadline_exceeded(status)) {
    iree_status_free(status);
  } else {
    IREE_EXPECT_OK(status);
  }
  EXPECT_EQ(completed, 0u);
  EXPECT_EQ(calls, 1);
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor_, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 1u);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ProgressTest, PollFailurePreservesOperationCompletions) {
  int operation_calls = 0;
  ProgressWork work([&](iree_host_size_t* completed) {
    *completed = 2;
    work.entry.remove_requested = true;
    return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
  });
  struct CompletionState {
    // Poll owner that admits the follow-up progress work.
    iree_async_proactor_t* proactor;
    // Progress work registered only after the operation completes.
    ProgressWork* work;
    // External completion witness.
    int* calls;
  } state{proactor_, &work, &operation_calls};
  iree_async_nop_operation_t operation = {};
  operation.base.type = IREE_ASYNC_OPERATION_TYPE_NOP;
  operation.base.user_data = &state;
  operation.base.completion_fn = +[](void* user_data, iree_async_operation_t*,
                                     iree_status_t status,
                                     iree_async_completion_flags_t) {
    auto* state = static_cast<CompletionState*>(user_data);
    IREE_EXPECT_OK(status);
    ++*state->calls;
    iree_async_proactor_register_progress(state->proactor, &state->work->entry);
  };
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));

  // Backends may dispatch the NOP before or after progress work within a poll.
  // The total must include it even if the same poll subsequently fails.
  iree_host_size_t total = 0;
  iree_status_t status = iree_ok_status();
  while (iree_status_is_ok(status)) {
    iree_host_size_t completed = 0;
    status = iree_async_proactor_poll(proactor_, iree_infinite_timeout(),
                                      &completed);
    total += completed;
  }
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE, status);
  EXPECT_EQ(total, 3u);
  EXPECT_EQ(operation_calls, 1);
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

}  // namespace

CTS_REGISTER_TEST_SUITE(ProgressTest);

}  // namespace iree::async::cts
