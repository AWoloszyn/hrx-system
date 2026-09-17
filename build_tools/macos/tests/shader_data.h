// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_BUILD_TOOLS_MACOS_TESTS_SHADER_DATA_H_
#define IREE_BUILD_TOOLS_MACOS_TESTS_SHADER_DATA_H_

#include "build_tools/macos/tests/shader.h"

// Returns embedded shader data through a separately linked library.
const iree_file_toc_t* iree_macos_test_shader_data(void);

#endif  // IREE_BUILD_TOOLS_MACOS_TESTS_SHADER_DATA_H_
