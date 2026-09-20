// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/bootstrap.h"

#include <string.h>

#include "iree/base/alignment.h"

// "IRSH" in little-endian byte order. Version covers the region and queue ABI
// as well as these records; there is no cross-version fallback.
#define IREE_NET_SHM_BOOTSTRAP_MAGIC 0x48535249u
#define IREE_NET_SHM_BOOTSTRAP_VERSION 2u

static void iree_net_shm_bootstrap_encode_header(
    iree_net_shm_bootstrap_type_t type, uint8_t* out_record) {
  iree_unaligned_store_le_u32(out_record + 0, IREE_NET_SHM_BOOTSTRAP_MAGIC);
  iree_unaligned_store_le_u16(out_record + 4, IREE_NET_SHM_BOOTSTRAP_VERSION);
  iree_unaligned_store_le_u16(out_record + 6, (uint16_t)type);
}

static iree_status_t iree_net_shm_bootstrap_decode_header(
    iree_const_byte_span_t record, iree_host_size_t expected_size,
    iree_net_shm_bootstrap_type_t expected_type) {
  if (record.data_length != expected_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM bootstrap record requires %" PRIhsz
                            " bytes; received %" PRIhsz,
                            expected_size, record.data_length);
  }
  if (iree_unaligned_load_le_u32(record.data + 0) !=
          IREE_NET_SHM_BOOTSTRAP_MAGIC ||
      iree_unaligned_load_le_u16(record.data + 4) !=
          IREE_NET_SHM_BOOTSTRAP_VERSION ||
      iree_unaligned_load_le_u16(record.data + 6) != expected_type) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM bootstrap magic, version, or phase mismatch");
  }
  return iree_ok_status();
}

void iree_net_shm_bootstrap_encode_offer(
    const iree_net_shm_region_layout_t* layout,
    uint8_t out_record[IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE]) {
  iree_net_shm_bootstrap_encode_header(IREE_NET_SHM_BOOTSTRAP_TYPE_OFFER,
                                       out_record);
  iree_unaligned_store_le_u32(out_record + 8, layout->options.endpoint_count);
  iree_unaligned_store_le_u32(out_record + 12, layout->options.slot_count);
  iree_unaligned_store_le_u32(out_record + 16, layout->options.slot_capacity);
}

iree_status_t iree_net_shm_bootstrap_decode_offer(
    iree_const_byte_span_t record, iree_net_shm_region_layout_t* out_layout) {
  memset(out_layout, 0, sizeof(*out_layout));
  IREE_RETURN_IF_ERROR(iree_net_shm_bootstrap_decode_header(
      record, IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE,
      IREE_NET_SHM_BOOTSTRAP_TYPE_OFFER));
  iree_net_shm_region_options_t options = {
      .endpoint_count = iree_unaligned_load_le_u32(record.data + 8),
      .slot_count = iree_unaligned_load_le_u32(record.data + 12),
      .slot_capacity = iree_unaligned_load_le_u32(record.data + 16),
  };
  return iree_net_shm_region_calculate_layout(options, out_layout);
}

void iree_net_shm_bootstrap_encode_ack(
    iree_net_shm_bootstrap_type_t type,
    uint8_t out_record[IREE_NET_SHM_BOOTSTRAP_HEADER_SIZE]) {
  iree_net_shm_bootstrap_encode_header(type, out_record);
}

iree_status_t iree_net_shm_bootstrap_decode_ack(
    iree_const_byte_span_t record,
    iree_net_shm_bootstrap_type_t expected_type) {
  return iree_net_shm_bootstrap_decode_header(
      record, IREE_NET_SHM_BOOTSTRAP_HEADER_SIZE, expected_type);
}
