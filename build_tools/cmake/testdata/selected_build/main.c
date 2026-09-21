// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>

int fixture_value(void);

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  FILE* input = fopen(argv[1], "r");
  if (!input) {
    return 2;
  }
  int expected_value = 0;
  int count = fscanf(input, "%d", &expected_value);
  fclose(input);
  if (count != 1) {
    return 2;
  }
  FILE* marker = fopen(argv[2], "a");
  if (!marker) {
    return 2;
  }
  fputs("run\n", marker);
  if (fclose(marker) != 0) {
    return 2;
  }
  return fixture_value() == expected_value ? 0 : 1;
}
