// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/proactor.h"

//===----------------------------------------------------------------------===//
// iree_async_proactor_initialize
//===----------------------------------------------------------------------===//

IREE_API_EXPORT void iree_async_proactor_initialize(
    const iree_async_proactor_vtable_t* vtable, iree_string_view_t debug_name,
    iree_allocator_t allocator, iree_async_proactor_t* out_proactor) {
  IREE_ASSERT_ARGUMENT(vtable);
  (void)debug_name;
  iree_atomic_ref_count_init(&out_proactor->ref_count);
  out_proactor->vtable = vtable;
  out_proactor->allocator = allocator;
  out_proactor->progress_list = NULL;
  out_proactor->cancellations.list = iree_intrusive_list_empty();
  out_proactor->cancellations.tail = NULL;
  IREE_TRACE({
    iree_host_size_t copy_length =
        iree_min(debug_name.size, sizeof(out_proactor->debug_name) - 1);
    if (copy_length > 0) {
      memcpy(out_proactor->debug_name, debug_name.data, copy_length);
    }
    out_proactor->debug_name[copy_length] = '\0';
  });
}

//===----------------------------------------------------------------------===//
// Caller-owned cancellation
//===----------------------------------------------------------------------===//

IREE_API_EXPORT void iree_async_cancel_request_initialize(
    iree_async_cancel_callback_t callback,
    iree_async_cancel_request_t* out_request) {
  memset(out_request, 0, sizeof(*out_request));
  out_request->callback = callback;
}

IREE_API_EXPORT iree_status_t iree_async_proactor_request_cancel(
    iree_async_proactor_t* proactor, iree_async_operation_t* target,
    iree_async_cancel_request_t* request) {
  if (!request->callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "cancellation receipt callback is required");
  }
  if (request->phase != IREE_ASYNC_CANCEL_REQUEST_PHASE_IDLE) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "cancellation request is already pending");
  }
  if (target->pool ||
      iree_any_bit_set(target->flags, IREE_ASYNC_OPERATION_FLAG_LINKED)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "owned cancellation requires a private, unlinked "
                            "target operation");
  }
  switch (target->type) {
    case IREE_ASYNC_OPERATION_TYPE_SOCKET_CONNECT:
    case IREE_ASYNC_OPERATION_TYPE_SOCKET_ACCEPT:
    case IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL:
      break;
    default:
      return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                              "owned cancellation is unsupported for operation "
                              "type %u",
                              (unsigned)target->type);
  }

  request->target = target;
  request->phase = IREE_ASYNC_CANCEL_REQUEST_PHASE_QUEUED;
  request->pending_entry.prev = proactor->cancellations.tail;
  request->pending_entry.next = NULL;
  if (proactor->cancellations.tail) {
    proactor->cancellations.tail->next = &request->pending_entry;
  } else {
    proactor->cancellations.list.head = &request->pending_entry;
  }
  proactor->cancellations.tail = &request->pending_entry;
  iree_async_proactor_wake(proactor);
  return iree_ok_status();
}

void iree_async_proactor_issue_cancel_request(
    iree_async_proactor_t* proactor, iree_async_cancel_request_t* request) {
  if (proactor->cancellations.tail == &request->pending_entry) {
    proactor->cancellations.tail = request->pending_entry.prev;
  }
  iree_intrusive_list_remove(&proactor->cancellations.list,
                             &request->pending_entry);
  request->target = NULL;
  request->phase = IREE_ASYNC_CANCEL_REQUEST_PHASE_ISSUED;
}

void iree_async_cancel_request_complete(iree_async_cancel_request_t* request) {
  iree_async_cancel_callback_t callback = request->callback;
  request->phase = IREE_ASYNC_CANCEL_REQUEST_PHASE_IDLE;
  callback.fn(callback.user_data);
}

IREE_API_EXPORT void iree_async_proactor_cancel_request_target_retired(
    iree_async_proactor_t* proactor, iree_async_cancel_request_t* request) {
  if (request->phase == IREE_ASYNC_CANCEL_REQUEST_PHASE_QUEUED) {
    iree_async_proactor_issue_cancel_request(proactor, request);
    iree_async_cancel_request_complete(request);
  }
}

//===----------------------------------------------------------------------===//
// Progress callback management
//===----------------------------------------------------------------------===//

IREE_API_EXPORT void iree_async_proactor_register_progress(
    iree_async_proactor_t* proactor, iree_async_progress_entry_t* entry) {
  IREE_ASSERT_ARGUMENT(proactor);
  IREE_ASSERT_ARGUMENT(entry);
  IREE_ASSERT_ARGUMENT(entry->fn);
  entry->remove_requested = false;
  entry->next = proactor->progress_list;
  proactor->progress_list = entry;
}

IREE_API_EXPORT void iree_async_proactor_unregister_progress(
    iree_async_proactor_t* proactor, iree_async_progress_entry_t* entry) {
  IREE_ASSERT_ARGUMENT(proactor);
  IREE_ASSERT_ARGUMENT(entry);
  iree_async_progress_entry_t** prev = &proactor->progress_list;
  while (*prev) {
    if (*prev == entry) {
      *prev = entry->next;
      entry->next = NULL;
      return;
    }
    prev = &(*prev)->next;
  }
}

IREE_API_EXPORT iree_status_t iree_async_proactor_run_progress(
    iree_async_proactor_t* proactor, iree_host_size_t* out_completed_count) {
  *out_completed_count = 0;
  if (!proactor->progress_list) {
    return iree_ok_status();
  }

  // Keep the unvisited suffix linked so callbacks may unregister and destroy
  // other entries. New and retained entries go before this stack cursor and
  // cannot run again in this pass. No caller-owned neighbor survives a callback
  // as an iterator, and each entry runs at most once per poll.
  iree_async_progress_entry_t cursor = {.next = proactor->progress_list};
  proactor->progress_list = &cursor;
  iree_status_t status = iree_ok_status();
  while (cursor.next && iree_status_is_ok(status)) {
    iree_async_progress_entry_t* entry = cursor.next;
    cursor.next = entry->next;
    entry->next = NULL;
    iree_host_size_t completed_count = 0;
    status = entry->fn(entry->user_data, &completed_count);
    *out_completed_count += completed_count;
    if (entry->remove_requested) {
      void (*on_remove)(void*) = entry->on_remove;
      void* on_remove_user_data = entry->user_data;
      entry->remove_requested = false;
      if (on_remove) {
        on_remove(on_remove_user_data);
      }
    } else {
      entry->next = proactor->progress_list;
      proactor->progress_list = entry;
    }
  }
  iree_async_proactor_unregister_progress(proactor, &cursor);
  return status;
}
