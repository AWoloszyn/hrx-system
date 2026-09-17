// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "iree/base/api.h"
#include "iree/net/status_wire.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  iree_status_t remote_status = iree_ok_status();
  iree_status_t parse_status = iree_net_status_wire_deserialize(
      iree_make_const_byte_span(data, size), &remote_status);
  if (!iree_status_is_ok(parse_status)) {
    iree_status_free(parse_status);
    iree_status_free(remote_status);
    return 0;
  }
  iree_status_free(parse_status);

  iree_host_size_t wire_size = 0;
  iree_status_t operation_status =
      iree_net_status_wire_calculate_size(remote_status, &wire_size);
  if (!iree_status_is_ok(operation_status)) {
    iree_status_free(remote_status);
    iree_status_abort(operation_status);
  }
  iree_status_free(operation_status);

  uint8_t* wire = static_cast<uint8_t*>(std::malloc(wire_size));
  if (!wire) {
    iree_status_free(remote_status);
    return 0;
  }
  operation_status = iree_net_status_wire_serialize(
      remote_status, iree_make_byte_span(wire, wire_size));
  if (!iree_status_is_ok(operation_status)) {
    iree_status_free(remote_status);
    std::free(wire);
    iree_status_abort(operation_status);
  }
  iree_status_free(operation_status);

  iree_status_t roundtrip_status = iree_ok_status();
  operation_status = iree_net_status_wire_deserialize(
      iree_make_const_byte_span(wire, wire_size), &roundtrip_status);
  if (!iree_status_is_ok(operation_status)) {
    iree_status_free(roundtrip_status);
    iree_status_free(remote_status);
    std::free(wire);
    iree_status_abort(operation_status);
  }
  iree_status_free(operation_status);
  if (iree_status_code(roundtrip_status) != iree_status_code(remote_status)) {
    iree_status_free(roundtrip_status);
    iree_status_free(remote_status);
    std::free(wire);
    std::abort();
  }
  iree_status_free(roundtrip_status);
  iree_status_free(remote_status);
  std::free(wire);
  return 0;
}
