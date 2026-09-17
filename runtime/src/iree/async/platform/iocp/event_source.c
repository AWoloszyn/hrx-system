// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/iocp/event_source.h"

#include <string.h>

#include "iree/async/platform/iocp/proactor.h"

#if defined(IREE_PLATFORM_WINDOWS)

// NTSTATUS is a LONG and non-negative values indicate success. Avoid including
// winternl.h, which conflicts with some Windows SDK header configurations.
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((LONG)(status)) >= 0)
#endif  // NT_SUCCESS

static iree_status_t iree_async_iocp_event_source_arm(
    iree_async_proactor_iocp_t* proactor,
    iree_async_event_source_t* event_source) {
  LONG already_signaled = FALSE;
  LONG nt_status = proactor->nt_wait_api.NtAssociateWaitCompletionPacket(
      (HANDLE)event_source->wait_packet_handle,
      (HANDLE)proactor->completion_port.handle,
      (HANDLE)event_source->target_handle,
      (PVOID)IREE_ASYNC_IOCP_EVENT_SOURCE_COMPLETION_KEY, (PVOID)event_source,
      0, 0, &already_signaled);
  if (!NT_SUCCESS(nt_status)) {
    return iree_make_status(
        IREE_STATUS_INTERNAL,
        "NtAssociateWaitCompletionPacket failed for event source "
        "(NTSTATUS 0x%08x)",
        (unsigned)nt_status);
  }
  return iree_ok_status();
}

static void iree_async_iocp_event_source_unlink(
    iree_async_proactor_iocp_t* proactor,
    iree_async_event_source_t* event_source) {
  if (event_source->previous) {
    event_source->previous->next = event_source->next;
  } else {
    proactor->event_sources = event_source->next;
  }
  if (event_source->next) {
    event_source->next->previous = event_source->previous;
  }
  event_source->next = NULL;
  event_source->previous = NULL;
}

// Cancels all kernel ownership before releasing |event_source|. Register and
// unregister are serialized with poll, so a failed cancellation would mean a
// completion was dequeued where the public contract says that is impossible.
static void iree_async_iocp_event_source_destroy(
    iree_async_proactor_iocp_t* proactor,
    iree_async_event_source_t* event_source) {
  LONG nt_status = proactor->nt_wait_api.NtCancelWaitCompletionPacket(
      (HANDLE)event_source->wait_packet_handle,
      /*remove_signaled_packet=*/TRUE);
  if (!NT_SUCCESS(nt_status)) {
    iree_status_abort(
        iree_make_status(IREE_STATUS_INTERNAL,
                         "NtCancelWaitCompletionPacket failed for event source "
                         "(NTSTATUS 0x%08x)",
                         (unsigned)nt_status));
  }
  if (!CloseHandle((HANDLE)event_source->wait_packet_handle)) {
    DWORD error = GetLastError();
    iree_status_abort(iree_make_status(
        iree_status_code_from_win32_error(error),
        "CloseHandle failed for event source wait packet (error %lu)",
        (unsigned long)error));
  }
  event_source->wait_packet_handle = 0;
  iree_async_iocp_event_source_unlink(proactor, event_source);
  iree_allocator_free(proactor->base.allocator, event_source);
}

iree_status_t iree_async_iocp_event_source_register(
    iree_async_proactor_t* base_proactor, iree_async_primitive_t primitive,
    iree_async_event_source_callback_t callback,
    iree_async_event_source_t** out_event_source) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_event_source);
  *out_event_source = NULL;

  if (primitive.type != IREE_ASYNC_PRIMITIVE_TYPE_WIN32_HANDLE) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "register_event_source requires a WIN32_HANDLE primitive (got type "
        "%d)",
        (int)primitive.type);
  }
  if (primitive.value.win32_handle == 0 ||
      primitive.value.win32_handle == (uintptr_t)INVALID_HANDLE_VALUE) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "register_event_source requires a valid HANDLE");
  }
  if (!callback.fn) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "register_event_source requires a callback");
  }

  iree_async_proactor_iocp_t* proactor =
      iree_async_proactor_iocp_cast(base_proactor);
  if (!proactor->nt_wait_api.available) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "IOCP event sources require wait completion packet support");
  }

  iree_async_event_source_t* event_source = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(proactor->base.allocator, sizeof(*event_source),
                                (void**)&event_source));
  memset(event_source, 0, sizeof(*event_source));
  event_source->target_handle = primitive.value.win32_handle;
  event_source->callback = callback;

  HANDLE wait_packet_handle = NULL;
  LONG nt_status = proactor->nt_wait_api.NtCreateWaitCompletionPacket(
      &wait_packet_handle, MAXIMUM_ALLOWED, NULL);
  iree_status_t status = iree_ok_status();
  if (!NT_SUCCESS(nt_status)) {
    status =
        iree_make_status(IREE_STATUS_INTERNAL,
                         "NtCreateWaitCompletionPacket failed for event source "
                         "(NTSTATUS 0x%08x)",
                         (unsigned)nt_status);
  } else {
    event_source->wait_packet_handle = (uintptr_t)wait_packet_handle;
    status = iree_async_iocp_event_source_arm(proactor, event_source);
  }

  if (iree_status_is_ok(status)) {
    event_source->next = proactor->event_sources;
    if (event_source->next) {
      event_source->next->previous = event_source;
    }
    proactor->event_sources = event_source;
    *out_event_source = event_source;
  } else {
    if (wait_packet_handle && !CloseHandle(wait_packet_handle)) {
      DWORD error = GetLastError();
      status = iree_status_join(
          status,
          iree_make_status(
              iree_status_code_from_win32_error(error),
              "CloseHandle failed while unwinding event source (error %lu)",
              (unsigned long)error));
    }
    iree_allocator_free(proactor->base.allocator, event_source);
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}

void iree_async_iocp_event_source_unregister(
    iree_async_proactor_t* base_proactor,
    iree_async_event_source_t* event_source) {
  if (!event_source) {
    return;
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_async_iocp_event_source_destroy(
      iree_async_proactor_iocp_cast(base_proactor), event_source);
  IREE_TRACE_ZONE_END(z0);
}

void iree_async_iocp_event_source_dispatch(
    iree_async_proactor_iocp_t* proactor,
    iree_async_event_source_t* event_source) {
  event_source->callback.fn(event_source->callback.user_data, event_source,
                            IREE_ASYNC_POLL_EVENT_IN);
  iree_status_t status =
      iree_async_iocp_event_source_arm(proactor, event_source);
  if (!iree_status_is_ok(status)) {
    iree_status_abort(status);
  }
}

void iree_async_iocp_event_source_deinitialize_all(
    iree_async_proactor_iocp_t* proactor) {
  while (proactor->event_sources) {
    iree_async_iocp_event_source_destroy(proactor, proactor->event_sources);
  }
}

#endif  // IREE_PLATFORM_WINDOWS
