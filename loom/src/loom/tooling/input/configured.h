// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_INPUT_CONFIGURED_H_
#define LOOM_TOOLING_INPUT_CONFIGURED_H_

#include "loom/tooling/input/input.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns immutable optional input providers selected by the build config.
// Final applications choose this composition; shared tooling accepts a list.
loom_input_provider_list_t loom_configured_input_providers(void);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // LOOM_TOOLING_INPUT_CONFIGURED_H_
