// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/frame_accumulator.h"

#include <string.h>

#include "iree/net/buffer_lease.h"

static iree_status_t iree_net_frame_accumulator_query_frame_size(
    iree_net_frame_accumulator_t* accumulator, iree_const_byte_span_t available,
    iree_host_size_t* out_frame_size) {
  *out_frame_size = 0;
  IREE_RETURN_IF_ERROR(accumulator->frame_length.fn(
      accumulator->frame_length.user_data, available, out_frame_size));
  if (*out_frame_size > accumulator->max_frame_size) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "frame size %" PRIhsz " exceeds max %" PRIhsz,
                            *out_frame_size, accumulator->max_frame_size);
  }
  return iree_ok_status();
}

static iree_status_t iree_net_frame_accumulator_begin_reassembly(
    iree_net_frame_accumulator_t* accumulator, iree_host_size_t frame_size) {
  if (frame_size < accumulator->header_used) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "resolved frame size %" PRIhsz
                            " is smaller than its %" PRIhsz "-byte prefix",
                            frame_size, accumulator->header_used);
  }

  iree_async_buffer_lease_t frame_lease;
  IREE_RETURN_IF_ERROR(iree_net_buffer_lease_allocate(
      frame_size, accumulator->host_allocator, &frame_lease));
  if (accumulator->header_used > 0) {
    memcpy(iree_async_span_ptr(frame_lease.span), accumulator->header,
           accumulator->header_used);
  }
  accumulator->frame_size = frame_size;
  accumulator->frame_used = accumulator->header_used;
  accumulator->frame_lease = frame_lease;
  accumulator->header_used = 0;
  return iree_ok_status();
}

static iree_status_t iree_net_frame_accumulator_complete_reassembly(
    iree_net_frame_accumulator_t* accumulator) {
  iree_const_byte_span_t frame = iree_make_const_byte_span(
      iree_async_span_ptr(accumulator->frame_lease.span),
      accumulator->frame_size);
  iree_status_t status = accumulator->on_frame_complete.fn(
      accumulator->on_frame_complete.user_data, frame,
      &accumulator->frame_lease);
  iree_async_buffer_lease_release(&accumulator->frame_lease);
  memset(&accumulator->frame_lease, 0, sizeof(accumulator->frame_lease));
  accumulator->frame_size = 0;
  accumulator->frame_used = 0;
  return status;
}

static iree_status_t iree_net_frame_accumulator_process(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data,
    iree_async_buffer_lease_t* lease) {
  if (data.length == 0) {
    return iree_ok_status();
  }
  if (!iree_async_span_is_cpu_accessible(data)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "frame parsing requires CPU-accessible storage");
  }

  const uint8_t* data_ptr = iree_async_span_ptr(data);
  if (!data_ptr) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "non-empty span has no storage");
  }
  iree_host_size_t remaining = data.length;

  while (remaining > 0) {
    if (accumulator->frame_size > 0) {
      iree_host_size_t copy_length = iree_min(
          remaining, accumulator->frame_size - accumulator->frame_used);
      memcpy(iree_async_span_ptr(accumulator->frame_lease.span) +
                 accumulator->frame_used,
             data_ptr, copy_length);
      accumulator->frame_used += copy_length;
      data_ptr += copy_length;
      remaining -= copy_length;
      if (accumulator->frame_used == accumulator->frame_size) {
        IREE_RETURN_IF_ERROR(
            iree_net_frame_accumulator_complete_reassembly(accumulator));
      }
      continue;
    }

    if (accumulator->header_used > 0) {
      accumulator->header[accumulator->header_used++] = *data_ptr++;
      --remaining;

      iree_host_size_t frame_size = 0;
      IREE_RETURN_IF_ERROR(iree_net_frame_accumulator_query_frame_size(
          accumulator,
          iree_make_const_byte_span(accumulator->header,
                                    accumulator->header_used),
          &frame_size));
      if (frame_size > 0) {
        IREE_RETURN_IF_ERROR(iree_net_frame_accumulator_begin_reassembly(
            accumulator, frame_size));
        if (accumulator->frame_used == accumulator->frame_size) {
          IREE_RETURN_IF_ERROR(
              iree_net_frame_accumulator_complete_reassembly(accumulator));
        }
      } else if (accumulator->header_used == accumulator->header_capacity) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "frame length unresolved after %" PRIhsz
                                " header bytes",
                                accumulator->header_capacity);
      }
      continue;
    }

    iree_host_size_t available_length =
        iree_min(remaining, accumulator->header_capacity);
    iree_host_size_t frame_size = 0;
    IREE_RETURN_IF_ERROR(iree_net_frame_accumulator_query_frame_size(
        accumulator, iree_make_const_byte_span(data_ptr, available_length),
        &frame_size));
    if (frame_size == 0) {
      if (available_length == accumulator->header_capacity) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "frame length unresolved after %" PRIhsz
                                " header bytes",
                                accumulator->header_capacity);
      }
      memcpy(accumulator->header, data_ptr, available_length);
      accumulator->header_used = available_length;
      data_ptr += available_length;
      remaining -= available_length;
      continue;
    }

    if (frame_size <= remaining) {
      iree_const_byte_span_t frame =
          iree_make_const_byte_span(data_ptr, frame_size);
      iree_async_buffer_lease_t* frame_lease =
          frame_size == remaining ? lease : NULL;
      data_ptr += frame_size;
      remaining -= frame_size;
      IREE_RETURN_IF_ERROR(accumulator->on_frame_complete.fn(
          accumulator->on_frame_complete.user_data, frame, frame_lease));
      continue;
    }

    IREE_RETURN_IF_ERROR(
        iree_net_frame_accumulator_begin_reassembly(accumulator, frame_size));
  }

  return iree_ok_status();
}

iree_status_t iree_net_frame_accumulator_initialize(
    iree_net_frame_accumulator_t* accumulator, iree_host_size_t max_frame_size,
    iree_net_frame_length_callback_t frame_length,
    iree_net_frame_complete_callback_t on_frame_complete,
    iree_allocator_t host_allocator) {
  IREE_ASSERT_ARGUMENT(accumulator);
  if (max_frame_size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "maximum frame size must be nonzero");
  }
  if (!frame_length.fn || frame_length.max_header_size == 0 ||
      !on_frame_complete.fn) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "frame callbacks and a nonzero maximum header size are required");
  }
  if (frame_length.max_header_size > max_frame_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "maximum header size %" PRIhsz
                            " exceeds maximum frame size %" PRIhsz,
                            frame_length.max_header_size, max_frame_size);
  }

  memset(accumulator, 0, sizeof(*accumulator));
  accumulator->frame_length = frame_length;
  accumulator->on_frame_complete = on_frame_complete;
  accumulator->host_allocator = host_allocator;
  accumulator->max_frame_size = max_frame_size;
  accumulator->header_capacity = frame_length.max_header_size;
  return iree_ok_status();
}

void iree_net_frame_accumulator_deinitialize(
    iree_net_frame_accumulator_t* accumulator) {
  IREE_ASSERT_ARGUMENT(accumulator);
  iree_net_frame_accumulator_reset(accumulator);
  memset(accumulator, 0, sizeof(*accumulator));
}

void iree_net_frame_accumulator_reset(
    iree_net_frame_accumulator_t* accumulator) {
  IREE_ASSERT_ARGUMENT(accumulator);
  iree_async_buffer_lease_release(&accumulator->frame_lease);
  memset(&accumulator->frame_lease, 0, sizeof(accumulator->frame_lease));
  accumulator->header_used = 0;
  accumulator->frame_size = 0;
  accumulator->frame_used = 0;
}

iree_status_t iree_net_frame_accumulator_push_span(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data) {
  IREE_ASSERT_ARGUMENT(accumulator);
  return iree_net_frame_accumulator_process(accumulator, data, NULL);
}

iree_status_t iree_net_frame_accumulator_push_lease(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data,
    iree_async_buffer_lease_t* lease) {
  IREE_ASSERT_ARGUMENT(accumulator);
  IREE_ASSERT_ARGUMENT(lease);

  iree_status_t status = iree_ok_status();
  if (data.region != lease->span.region || data.offset < lease->span.offset) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "data span is outside of its buffer lease");
  } else {
    iree_host_size_t relative_offset = data.offset - lease->span.offset;
    if (relative_offset > lease->span.length ||
        data.length > lease->span.length - relative_offset) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "data span is outside of its buffer lease");
    }
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_frame_accumulator_process(accumulator, data, lease);
  }
  iree_async_buffer_lease_release(lease);
  return status;
}
