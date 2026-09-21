// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "iree/base/api.h"
#include "iree/net/carrier/shm/bootstrap.h"

static void CheckOffer(iree_const_byte_span_t record) {
  iree_net_shm_region_layout_t layout;
  iree_status_t status = iree_net_shm_bootstrap_decode_offer(record, &layout);
  if (!iree_status_is_ok(status)) {
    iree_status_free(status);
    if (layout.total_size != 0) {
      std::abort();
    }
    return;
  }
  iree_status_free(status);
  uint8_t encoded[IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE];
  iree_net_shm_bootstrap_encode_offer(&layout, encoded);
  if (std::memcmp(record.data, encoded, sizeof(encoded)) != 0 ||
      layout.total_size < layout.direction_stride ||
      layout.payload_offset >= layout.direction_stride ||
      layout.slot_stride < layout.options.slot_capacity ||
      layout.slot_stride % IREE_NET_SHM_REGION_ALIGNMENT != 0) {
    std::abort();
  }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const auto record = iree_make_const_byte_span(data, size);
  CheckOffer(record);
  iree_status_free(iree_net_shm_bootstrap_decode_ack(
      record, IREE_NET_SHM_BOOTSTRAP_TYPE_ACCEPT));
  iree_status_free(iree_net_shm_bootstrap_decode_ack(
      record, IREE_NET_SHM_BOOTSTRAP_TYPE_READY));

  // Exercise geometry independently of finding a valid magic/version header.
  // No input controls an allocation or mapping, even for very large extents.
  iree_net_shm_region_options_t options = {1, 1, 1};
  std::memcpy(&options, data, size < sizeof(options) ? size : sizeof(options));
  iree_net_shm_region_layout_t layout;
  iree_status_t status = iree_net_shm_region_calculate_layout(options, &layout);
  if (iree_status_is_ok(status)) {
    uint8_t encoded[IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE];
    iree_net_shm_bootstrap_encode_offer(&layout, encoded);
    CheckOffer(iree_make_const_byte_span(encoded, sizeof(encoded)));
  }
  iree_status_free(status);
  return 0;
}
