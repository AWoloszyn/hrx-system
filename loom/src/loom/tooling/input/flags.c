// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/input/flags.h"

#include "iree/base/tooling/flags.h"

IREE_FLAG_NAMED(string, input_format, "input-format", "",
                "Source input format. Empty selects from each filename. "
                "Bytecode is detected by file magic.");
IREE_FLAG_LIST_NAMED(string, input_options, "input-options",
                     "Options for an input provider: 'format:key=value ...'. "
                     "Repeat once per format, for example "
                     "--input-options='cxx:std=c++20 I=include'.");
IREE_FLAG_LIST_NAMED(string, source_prefix_map, "source-prefix-map",
                     "Remap diagnostic filenames with old=new. Repeat for "
                     "multiple prefixes; the last matching entry wins.");

loom_input_options_t loom_input_options_from_flags(void) {
  return (loom_input_options_t){
      .format = iree_make_cstring_view(FLAG_input_format),
      .provider_options = FLAG_input_options_list(),
      .source_path_options = {.prefix_maps = FLAG_source_prefix_map_list()},
  };
}
