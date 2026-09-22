// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

static bool read(const std::string& name,
                 const std::string& expected = "payload") {
  std::ifstream source(name);
  std::string value;
  if (!std::getline(source, value) || value != expected) {
    std::cerr << "Cannot read payload: " << name << '\n';
    return false;
  }
  std::cout << value << std::endl;
  return true;
}
int main(int argc, char** argv) {
  bool valid = true;
  if (const char* name = std::getenv("TEST_INPUT")) {
    valid = read(name);
  }
  for (int index = 1; index < argc; ++index) {
    std::string argument(argv[index]);
    std::cout << "ARG: " << argument << '\n';
    if (argument == "--check-suppression") {
      const char* environment = std::getenv("LSAN_OPTIONS");
      std::string options(environment ? environment : "");
      std::string prefix = "suppressions=";
      std::string suffix = ":allow_addr2line=1";
      auto suffix_position = options.rfind(suffix);
      valid =
          options.rfind(prefix, 0) == 0 &&
          suffix_position != std::string::npos &&
          suffix_position + suffix.size() == options.size() &&
          read(options.substr(prefix.size(), suffix_position - prefix.size()),
               "leak:fixture") &&
          valid;
    } else if (argument.rfind("--literal=", 0) == 0) {
      valid = (argument == "--literal=fixture.txt") && valid;
    } else if (argument.rfind("--input=", 0) == 0) {
      valid = read(argument.substr(8)) && valid;
    } else if (argument.rfind("--wrapped=[", 0) == 0) {
      valid = (argument.back() == ']') &&
              read(argument.substr(11, argument.size() - 12)) && valid;
    } else if (argument.rfind("--pair=", 0) == 0) {
      auto separator = argument.find('|');
      valid = (separator != std::string::npos) &&
              read(argument.substr(7, separator - 7)) &&
              read(argument.substr(separator + 1)) && valid;
    } else {
      valid = read(argument) && valid;
    }
  }
  return valid ? 0 : 1;
}
