// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "build_tools/macos/tests/shader_data.h"

const iree_file_toc_t* iree_macos_test_shader_data(void) {
  return iree_macos_test_shader_create();
}
