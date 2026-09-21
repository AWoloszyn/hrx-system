// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/posix/proactor.h"

iree_status_t iree_async_proactor_posix_drain_cancel_requests(
    iree_async_proactor_posix_t* proactor,
    iree_host_size_t* inout_completed_count) {
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < IREE_ASYNC_CANCEL_REQUEST_SERVICE_BUDGET &&
       proactor->base.cancellations.list.head && iree_status_is_ok(status);
       ++i) {
    iree_async_cancel_request_t* request =
        (iree_async_cancel_request_t*)proactor->base.cancellations.list.head;
    iree_async_operation_t* target = request->target;
    int fd = iree_async_proactor_posix_operation_fd(target);
    iree_async_posix_fd_handler_type_t handler_type;
    void* handler = NULL;
    bool found = false;
    short remaining_events = 0;
    if (iree_async_posix_fd_map_lookup(&proactor->fd_map, fd, &handler_type,
                                       &handler) &&
        handler_type == IREE_ASYNC_POSIX_FD_HANDLER_OPERATION) {
      for (iree_async_operation_t* operation = handler; operation;
           operation = operation->next) {
        if (operation == target) {
          found = true;
        } else {
          remaining_events |=
              iree_async_proactor_posix_operation_poll_events(operation);
        }
      }
    }
    if (!found) {
      // The owner may have just submitted the target from a callback. Its
      // pending registration or already-published completion must drain before
      // this request can retire it. Neither case depends on peer readiness.
      break;
    }

    // Preserve map ownership until native interest removal succeeds. Other
    // operations on this descriptor retain exactly their original interests.
    status = remaining_events
                 ? iree_async_posix_event_set_modify(proactor->event_set, fd,
                                                     remaining_events)
                 : iree_async_posix_event_set_remove(proactor->event_set, fd);
    if (iree_status_is_ok(status)) {
      iree_async_posix_fd_map_remove_operation(&proactor->fd_map, fd, target);
      target->next = NULL;
      iree_async_proactor_issue_cancel_request(&proactor->base, request);
      ++*inout_completed_count;
      iree_async_cancel_request_complete(request);
      *inout_completed_count += iree_async_proactor_posix_complete_direct(
          proactor, target, iree_status_from_code(IREE_STATUS_CANCELLED),
          IREE_ASYNC_COMPLETION_FLAG_NONE);
    }
  }
  if (proactor->base.cancellations.list.head) {
    iree_async_proactor_posix_wake_poll_thread(proactor);
  }
  return status;
}
