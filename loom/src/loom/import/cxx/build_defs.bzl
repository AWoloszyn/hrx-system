# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Compiler options for parser-facing C++ importer packages."""

CXX_COMPILER_OPTIONS = {
    "@platforms//os:windows": ["/std:c++latest", "/EHsc", "/GR"],
    "//conditions:default": ["-std=c++23", "-fexceptions", "-frtti"],
}
