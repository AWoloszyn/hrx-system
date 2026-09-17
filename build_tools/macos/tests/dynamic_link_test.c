// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "build_tools/macos/tests/shader_data.h"

int main(void) {
  const iree_file_toc_t* source = iree_macos_test_shader_data();
  if (!source->data || !source->size) {
    fprintf(stderr, "The shared library returned no embedded shader data\n");
    return 1;
  }
  const void* symbols[] = {(const void*)iree_macos_test_shader_data,
                           (const void*)iree_macos_test_shader_create};
  for (size_t symbol_index = 0;
       symbol_index < sizeof(symbols) / sizeof(symbols[0]); ++symbol_index) {
    Dl_info library_info = {0};
    if (!dladdr(symbols[symbol_index], &library_info)) {
      fprintf(
          stderr,
          "Could not find the loaded image containing the shader library\n");
      return 1;
    }
    const char* extension = strrchr(library_info.dli_fname, '.');
    if (!extension || strcmp(extension, ".dylib") != 0) {
      fprintf(stderr, "Expected a shared shader library, found %s\n",
              library_info.dli_fname);
      return 1;
    }
  }
  return 0;
}
