// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/operations/net.h"
#include "iree/async/platform/iocp/proactor.h"

#if defined(IREE_PLATFORM_WINDOWS)

// NtCancelWaitCompletionPacket distinguishes successful withdrawal from a
// pending or already-dequeued completion. NT_SUCCESS includes STATUS_PENDING
// and therefore cannot establish that the carrier is safe to recycle.
#define IREE_ASYNC_IOCP_STATUS_PENDING ((LONG)0x00000103L)
#define IREE_ASYNC_IOCP_STATUS_CANCELLED ((LONG)0xC0000120L)

iree_status_t iree_async_proactor_iocp_cancel_wait(
    iree_async_proactor_iocp_t* proactor, iree_async_iocp_carrier_t* carrier,
    bool* out_withdrawn) {
  *out_withdrawn = false;
  HANDLE wait_handle = carrier->data.event_wait.wait_handle;
  if (!wait_handle) {
    return iree_ok_status();
  }

  if (proactor->nt_wait_api.available) {
    LONG result =
        proactor->nt_wait_api.NtCancelWaitCompletionPacket(wait_handle, TRUE);
    if (result != 0 && result != IREE_ASYNC_IOCP_STATUS_PENDING &&
        result != IREE_ASYNC_IOCP_STATUS_CANCELLED) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "NtCancelWaitCompletionPacket failed: 0x%08lx",
                              (unsigned long)result);
    }
    if (!CloseHandle(wait_handle)) {
      iree_abort();
    }
    *out_withdrawn = result == 0;
  } else {
    // Only joins the short callback that publishes a completion. It never
    // waits for peer signaling or poll progress and cannot form a poll cycle.
    if (!UnregisterWaitEx(wait_handle, INVALID_HANDLE_VALUE)) {
      DWORD error = GetLastError();
      return iree_make_status(iree_status_code_from_win32_error(error),
                              "UnregisterWaitEx failed: %lu",
                              (unsigned long)error);
    }
    *out_withdrawn = iree_atomic_load(&carrier->fallback_completion_state,
                                      iree_memory_order_acquire) ==
                     IREE_ASYNC_IOCP_FALLBACK_COMPLETION_NONE;
  }
  carrier->data.event_wait.wait_handle = NULL;
  return iree_ok_status();
}

iree_status_t iree_async_proactor_iocp_drain_cancel_requests(
    iree_async_proactor_iocp_t* proactor, iree_host_size_t* completed_count) {
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < IREE_ASYNC_CANCEL_REQUEST_SERVICE_BUDGET &&
       proactor->base.cancellations.list.head && iree_status_is_ok(status);
       ++i) {
    iree_async_cancel_request_t* request =
        (iree_async_cancel_request_t*)proactor->base.cancellations.list.head;
    iree_async_operation_t* target = request->target;
    bool is_wait = target->type == IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL;
    if (is_wait &&
        !iree_any_bit_set(iree_async_operation_load_internal_flags(target),
                          IREE_ASYNC_IOCP_INTERNAL_FLAG_WAIT_REGISTERED)) {
      // A callback may have submitted a wait after the last pending drain.
      // Its next pointer is queue linkage until registration establishes the
      // carrier identity. Preserve the request until that owner work drains.
      break;
    }
    iree_async_iocp_carrier_t* carrier =
        (iree_async_iocp_carrier_t*)target->next;
    bool withdrawn = false;
    if (is_wait) {
      status =
          iree_async_proactor_iocp_cancel_wait(proactor, carrier, &withdrawn);
    } else if (carrier) {
      if (!CancelIoEx((HANDLE)carrier->io_handle, &carrier->overlapped)) {
        DWORD error = GetLastError();
        if (error != ERROR_NOT_FOUND) {
          status =
              iree_make_status(iree_status_code_from_win32_error(error),
                               "CancelIoEx failed: %lu", (unsigned long)error);
        }
      }
    }
    if (iree_status_is_ok(status)) {
      // This also suppresses emulated multishot rearm when an already-posted
      // accept completion won the native CancelIoEx race.
      if (!is_wait) {
        iree_async_operation_set_internal_flags(
            target, IREE_ASYNC_IOCP_INTERNAL_FLAG_CANCELLED);
      }
      iree_async_proactor_issue_cancel_request(&proactor->base, request);
      if (withdrawn) {
        if (carrier->prev) {
          carrier->prev->next = carrier->next;
        } else {
          proactor->active_carriers = carrier->next;
        }
        if (carrier->next) {
          carrier->next->prev = carrier->prev;
        }
        target->next = NULL;
        iree_async_proactor_iocp_release_carrier(proactor, carrier);
      }
      // CancelIoEx and native wait unregistration do not borrow the target key
      // after returning. An already-published target completion still owns its
      // carrier and target until normal dispatch, independently of this
      // receipt.
      ++*completed_count;
      iree_async_cancel_request_complete(request);
      if (withdrawn) {
        iree_async_proactor_iocp_dispatch_completion(
            proactor, target, iree_status_from_code(IREE_STATUS_CANCELLED),
            IREE_ASYNC_COMPLETION_FLAG_NONE, completed_count);
      }
    }
  }
  if (proactor->base.cancellations.list.head) {
    iree_async_proactor_wake(&proactor->base);
  }
  return status;
}

#endif  // IREE_PLATFORM_WINDOWS
