// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/cts/util/registry.h"
#include "iree/async/proactor.h"
#include "iree/base/api.h"

namespace iree::async::cts {
namespace {

iree::StatusOr<iree_async_proactor_t*> CreateUnavailableProactor(
    iree_async_proactor_options_t options) {
  return iree::Status(iree_make_status(IREE_STATUS_UNAVAILABLE,
                                       "benchmark backend unavailable"));
}

iree::StatusOr<iree_async_proactor_t*> CreateFailedProactor(
    iree_async_proactor_options_t options) {
  return iree::Status(iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                       "benchmark backend allocation failed"));
}

// Exercise every linked benchmark's actual setup and result reporting without
// depending on the host's native backend availability.
static bool backends_registered_ =
    (CtsRegistry::RegisterBackend(
         {"unavailable", {"unavailable", CreateUnavailableProactor}, {}}),
     CtsRegistry::RegisterBackend(
         {"failed", {"failed", CreateFailedProactor}, {}}),
     true);

}  // namespace
}  // namespace iree::async::cts
