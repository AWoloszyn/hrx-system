// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/licenses/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_MEMORY_PAIR_H_
#define AMDF_SRC_MEMORY_PAIR_H_

#include "amdf/amdf.h"

// Provider-neutral facts for one concrete memory access site.
typedef struct amdf_memory_pair_query_site_t {
  // Engine family owning the attachment.
  amdf_engine_kind_t engine_kind;
  // Exact public queue-family ordinal named by the execution site.
  uint32_t queue_family_ordinal;
  // Borrowed immutable properties of the attachment.
  const amdf_memory_info_t* memory_info;
} amdf_memory_pair_query_site_t;

// Provider-neutral inputs to one exact directional pair query.
typedef struct amdf_memory_pair_query_t {
  // Site that produces data before the reported transition.
  amdf_memory_pair_query_site_t producer;
  // Site that consumes data after the reported transition.
  amdf_memory_pair_query_site_t consumer;
} amdf_memory_pair_query_t;

#endif  // AMDF_SRC_MEMORY_PAIR_H_
