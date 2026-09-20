// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/input/configured.h"

#ifndef LOOM_CONFIG_INPUT_HAVE_CXX
#define LOOM_CONFIG_INPUT_HAVE_CXX 0
#endif  // LOOM_CONFIG_INPUT_HAVE_CXX

#if LOOM_CONFIG_INPUT_HAVE_CXX
#include "loom/import/cxx/tooling/input.h"
#endif  // LOOM_CONFIG_INPUT_HAVE_CXX

loom_input_provider_list_t loom_configured_input_providers(void) {
#if LOOM_CONFIG_INPUT_HAVE_CXX
  static const loom_input_provider_t* const providers[] = {
      &loom_cxx_input_provider,
  };
  return (loom_input_provider_list_t){providers, IREE_ARRAYSIZE(providers)};
#else
  return (loom_input_provider_list_t){0};
#endif  // LOOM_CONFIG_INPUT_HAVE_CXX
}
