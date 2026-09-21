// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/transport_factory.h"

IREE_API_EXPORT void iree_net_transport_connect_operation_initialize(
    iree_net_transport_connect_operation_t* operation) {
  iree_slim_mutex_initialize(&operation->mutex);
  operation->binding.cancel_fn = NULL;
  operation->binding.user_data = NULL;
}

IREE_API_EXPORT void iree_net_transport_connect_operation_deinitialize(
    iree_net_transport_connect_operation_t* operation) {
  iree_slim_mutex_deinitialize(&operation->mutex);
}

IREE_API_EXPORT void iree_net_transport_connect_operation_cancel(
    iree_net_transport_connect_operation_t* operation) {
  iree_slim_mutex_lock(&operation->mutex);
  if (operation->binding.cancel_fn) {
    operation->binding.cancel_fn(operation->binding.user_data);
  }
  iree_slim_mutex_unlock(&operation->mutex);
}
