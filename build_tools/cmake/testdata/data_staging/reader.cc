// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    std::ifstream source(argv[index]);
    if (!source) {
      std::cerr << "Cannot read " << argv[index] << '\n';
      return 1;
    }
    std::cout << source.rdbuf();
  }
  return 0;
}
