// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// SHM bootstrap records carried by async/util/local_stream.
//
// The server creates the mapping and both directional native wakes and sends
// OFFER with their handles. The client retains tentative imports, checks the
// scalar offer, and sends ACCEPT. READY confirms that the server held the
// offered resources through ACCEPT. Only then may the client map resources,
// register native wakes, and construct its connection. Client setup failure
// closes the stream; after publication it stays open to observe peer departure.
//
// All fields are little-endian. The 8-byte header is magic (u32), version
// (u16), and type (u16). OFFER appends endpoint count, slot count, and slot
// capacity (three u32s). ACCEPT and READY contain only the header. Native
// handle identity and count come from the platform, not a wire PID or pointer.
// No native objects are created or imported by this codec.

#ifndef IREE_NET_CARRIER_SHM_BOOTSTRAP_H_
#define IREE_NET_CARRIER_SHM_BOOTSTRAP_H_

#include "iree/base/api.h"
#include "iree/net/carrier/shm/region.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define IREE_NET_SHM_BOOTSTRAP_HEADER_SIZE 8u
#define IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE 20u

// Exact-version handshake phases; each peer expects one specific next record.
typedef enum iree_net_shm_bootstrap_type_e {
  IREE_NET_SHM_BOOTSTRAP_TYPE_OFFER = 1,
  IREE_NET_SHM_BOOTSTRAP_TYPE_ACCEPT = 2,
  IREE_NET_SHM_BOOTSTRAP_TYPE_READY = 3,
} iree_net_shm_bootstrap_type_t;

// Encodes an OFFER from previously validated geometry into exactly 20 bytes.
void iree_net_shm_bootstrap_encode_offer(
    const iree_net_shm_region_layout_t* layout,
    uint8_t out_record[IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE]);

// Validates an exact OFFER and derives its checked region geometry. The caller
// applies local resource policy before allocating views or importing handles.
// |out_layout| is zero on failure.
iree_status_t iree_net_shm_bootstrap_decode_offer(
    iree_const_byte_span_t record, iree_net_shm_region_layout_t* out_layout);

// Encodes an ACCEPT or READY record. |type| is the locally selected next phase,
// not a peer-provided value.
void iree_net_shm_bootstrap_encode_ack(
    iree_net_shm_bootstrap_type_t type,
    uint8_t out_record[IREE_NET_SHM_BOOTSTRAP_HEADER_SIZE]);

// Validates an exact ACCEPT or READY record against the local handshake phase.
iree_status_t iree_net_shm_bootstrap_decode_ack(
    iree_const_byte_span_t record, iree_net_shm_bootstrap_type_t expected_type);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_BOOTSTRAP_H_
