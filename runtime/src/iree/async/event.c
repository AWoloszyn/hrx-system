// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/event.h"

#include "iree/async/proactor.h"

IREE_API_EXPORT iree_status_t iree_async_event_create(
    iree_async_proactor_t* proactor, iree_async_event_t** out_event) {
  IREE_ASSERT_ARGUMENT(proactor);
  IREE_ASSERT_ARGUMENT(out_event);
  *out_event = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_status_t status = proactor->vtable->create_event(proactor, out_event);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

// Destroys the event, releasing backend resources and closing platform handles.
// Routed through the proactor that created this event.
static void iree_async_event_destroy(iree_async_event_t* event) {
  IREE_TRACE_ZONE_BEGIN(z0);
  event->proactor->vtable->destroy_event(event->proactor, event);
  IREE_TRACE_ZONE_END(z0);
}

IREE_API_EXPORT void iree_async_event_retain(iree_async_event_t* event) {
  if (event) {
    iree_atomic_ref_count_inc(&event->ref_count);
  }
}

IREE_API_EXPORT void iree_async_event_release(iree_async_event_t* event) {
  if (event && iree_atomic_ref_count_dec(&event->ref_count) == 1) {
    iree_async_event_destroy(event);
  }
}

IREE_API_EXPORT iree_status_t iree_async_event_set(iree_async_event_t* event) {
  return iree_async_event_native_set(&event->native);
}

#if defined(IREE_PLATFORM_LINUX) || defined(IREE_PLATFORM_ANDROID)

#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

IREE_API_EXPORT iree_status_t
iree_async_event_native_initialize(iree_async_event_native_t* out_event) {
  memset(out_event, 0, sizeof(*out_event));
  int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    return iree_make_status(iree_status_code_from_errno(errno),
                            "eventfd creation failed");
  }
  out_event->wait_primitive = iree_async_primitive_from_fd(fd);
  out_event->signal_primitive = out_event->wait_primitive;
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t
iree_async_event_native_set(const iree_async_event_native_t* event) {
  // eventfd: write a 64-bit value to signal. The kernel accumulates values
  // until read. Writing 1 is idiomatic; the actual value doesn't matter for
  // our binary signal semantics.
  uint64_t value = 1;
  ssize_t result = 0;
  do {
    result = write(event->signal_primitive.value.fd, &value, sizeof(value));
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    // EAGAIN means the counter would overflow (saturated). That's fine - the
    // event is already maximally signaled. Any other error is unexpected.
    if (errno != EAGAIN) {
      return iree_make_status(iree_status_code_from_errno(errno),
                              "eventfd write failed");
    }
  }
  return iree_ok_status();
}

#elif defined(IREE_PLATFORM_APPLE) || defined(IREE_PLATFORM_BSD)

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

IREE_API_EXPORT iree_status_t
iree_async_event_native_initialize(iree_async_event_native_t* out_event) {
  memset(out_event, 0, sizeof(*out_event));
  int pipe_fds[2];
  if (pipe(pipe_fds) < 0) {
    return iree_make_status(iree_status_code_from_errno(errno),
                            "pipe creation failed for event");
  }
  iree_status_t status = iree_ok_status();
  for (int i = 0; i < 2 && iree_status_is_ok(status); ++i) {
    int flags = fcntl(pipe_fds[i], F_GETFL);
    if (flags < 0 || fcntl(pipe_fds[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(pipe_fds[i], F_SETFD, FD_CLOEXEC) < 0) {
      status = iree_make_status(iree_status_code_from_errno(errno),
                                "setting native event pipe flags");
    }
  }
  if (iree_status_is_ok(status)) {
    out_event->wait_primitive = iree_async_primitive_from_fd(pipe_fds[0]);
    out_event->signal_primitive = iree_async_primitive_from_fd(pipe_fds[1]);
  } else {
    close(pipe_fds[1]);
    close(pipe_fds[0]);
  }
  return status;
}

IREE_API_EXPORT iree_status_t
iree_async_event_native_set(const iree_async_event_native_t* event) {
  // pipe: write a single byte to signal. The read end becomes readable.
  uint8_t value = 1;
  ssize_t result = 0;
  do {
    result = write(event->signal_primitive.value.fd, &value, sizeof(value));
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    // EAGAIN means the pipe buffer is full. That's fine - the event is already
    // signaled (there's data to read).
    if (errno != EAGAIN) {
      return iree_make_status(iree_status_code_from_errno(errno),
                              "pipe write failed");
    }
  }
  return iree_ok_status();
}

#elif defined(IREE_PLATFORM_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif  // WIN32_LEAN_AND_MEAN
#include <windows.h>

IREE_API_EXPORT iree_status_t
iree_async_event_native_initialize(iree_async_event_native_t* out_event) {
  memset(out_event, 0, sizeof(*out_event));
  HANDLE handle = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (!handle) {
    return iree_make_status(iree_status_code_from_win32_error(GetLastError()),
                            "CreateEvent failed");
  }
  out_event->wait_primitive =
      iree_async_primitive_from_win32_handle((uintptr_t)handle);
  out_event->signal_primitive = out_event->wait_primitive;
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t
iree_async_event_native_set(const iree_async_event_native_t* event) {
  // Win32 event: SetEvent signals the event. If it's already signaled, this
  // is a no-op (for manual-reset events) or still succeeds (for auto-reset).
  HANDLE handle = (HANDLE)event->signal_primitive.value.win32_handle;
  if (!SetEvent(handle)) {
    return iree_make_status(iree_status_code_from_win32_error(GetLastError()),
                            "SetEvent failed");
  }
  return iree_ok_status();
}

#else  // unsupported platform

IREE_API_EXPORT iree_status_t
iree_async_event_native_initialize(iree_async_event_native_t* out_event) {
  memset(out_event, 0, sizeof(*out_event));
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "native events not supported on this platform");
}

IREE_API_EXPORT iree_status_t
iree_async_event_native_set(const iree_async_event_native_t* event) {
  (void)event;
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "events not supported on this platform");
}

#endif  // platform

IREE_API_EXPORT void iree_async_event_native_deinitialize(
    iree_async_event_native_t* event) {
#if defined(IREE_ASYNC_HAVE_FD)
  if (event->wait_primitive.type == event->signal_primitive.type &&
      event->wait_primitive.value.fd == event->signal_primitive.value.fd) {
    event->signal_primitive = iree_async_primitive_none();
  }
#elif defined(IREE_ASYNC_HAVE_WIN32_HANDLE)
  if (event->wait_primitive.type == event->signal_primitive.type &&
      event->wait_primitive.value.win32_handle ==
          event->signal_primitive.value.win32_handle) {
    event->signal_primitive = iree_async_primitive_none();
  }
#endif  // native handle type
  iree_async_primitive_close(&event->signal_primitive);
  iree_async_primitive_close(&event->wait_primitive);
}
