// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/options.h"

void loom_cxx_import_options_initialize(loom_cxx_import_options_t* options) {
  memset(options, 0, sizeof(*options));
  options->standard = IREE_SV("c++26");
  options->data_model = LOOM_CXX_DATA_MODEL_LP64;
}
