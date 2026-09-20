// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_INPUT_FLAGS_H_
#define LOOM_TOOLING_INPUT_FLAGS_H_

#include "loom/tooling/input/input.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns borrowed source admission options from the parsed command line.
loom_input_options_t loom_input_options_from_flags(void);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // LOOM_TOOLING_INPUT_FLAGS_H_
