// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* environment_value(const char* name) {
  const char* value = getenv(name);
  return value ? value : "absent";
}

int main(int argc, char** argv) {
  if (argc != 5 || strcmp(argv[1], "argument with spaces") ||
      strcmp(argv[2], "literal=[value]") ||
      strcmp(environment_value("IREE_TEST_COMMAND_ENV"),
             "authored environment")) {
    fprintf(stderr,
            "Test arguments or authored environment were not preserved\n");
    return 1;
  }
  if (strcmp(environment_value("IREE_TEST_COMMAND_EMULATOR"), argv[3]) ||
      strcmp(environment_value("IREE_TEST_COMMAND_LAUNCHER"), argv[4])) {
    fprintf(stderr,
            "Executable target's execution prefixes were not applied\n");
    return 1;
  }
  return 0;
}
