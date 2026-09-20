// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/factory.h"
#include "iree/net/cts/transport_backend.h"
#include "iree/testing/temp_file.h"

namespace iree::net::cts {
namespace {

iree_status_t CreateFactory(iree_allocator_t host_allocator,
                            iree_net_transport_factory_t** out_factory) {
  return iree_net_shm_factory_create(nullptr, host_allocator, out_factory);
}

iree_status_t MakeBindAddress(std::string* out_address) {
  *out_address = iree::testing::MakeTempFilePath("shm");
  // Native local names have small length limits independent of the test output
  // directory. Filesystem sockets live in the test's working directory.
  *out_address = out_address->substr(out_address->find_last_of("\\/") + 1);
#if defined(IREE_PLATFORM_LINUX) || defined(IREE_PLATFORM_ANDROID)
  *out_address = "@" + *out_address;
#endif
  return iree_ok_status();
}

}  // namespace

const TransportBackend& GetTransportBackend() {
  static const TransportBackend backend = {
      "shm",
      IREE_NET_TRANSPORT_CAPABILITY_RELIABLE |
          IREE_NET_TRANSPORT_CAPABILITY_ORDERED,
      CreateFactory,
      MakeBindAddress,
  };
  return backend;
}

}  // namespace iree::net::cts
