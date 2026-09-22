# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Sanitizer suppression sets owned by external runtime dependencies."""

def sanitizer_suppression_options(sanitizer, path):
    """Returns the sanitizer environment name and options for a suppression file.

    Args:
      sanitizer: Validated sanitizer name.
      path: Suppression file path, optionally containing runfile markers.

    Returns:
      A pair containing the environment name and option string.
    """
    if sanitizer not in _SANITIZER_OPTION_ENV:
        fail("unknown sanitizer suppression kind: %s" % sanitizer)

    # Sanitizer flag separators include spaces and colons on every platform.
    options = ['suppressions="' + path + '"']
    options.extend(_SANITIZER_EXTRA_OPTIONS.get(sanitizer, []))
    return _SANITIZER_OPTION_ENV[sanitizer], ":".join(options)

hsa_suppressions = {
    "lsan": "//build_tools/sanitizer:lsan_suppressions_hsa.txt",
}

rocm_suppressions = {
    "lsan": "//build_tools/sanitizer:lsan_suppressions_rocm.txt",
}

vulkan_suppressions = {
    "lsan": "//build_tools/sanitizer:lsan_suppressions_vulkan.txt",
}

_SANITIZER_OPTION_ENV = {
    "asan": "ASAN_OPTIONS",
    "lsan": "LSAN_OPTIONS",
    "msan": "MSAN_OPTIONS",
    "tsan": "TSAN_OPTIONS",
    "ubsan": "UBSAN_OPTIONS",
}

_SANITIZER_EXTRA_OPTIONS = {
    # LSAN suppression matching depends on symbolized stack frames. Bazel test
    # sandboxes may not expose llvm-symbolizer on PATH, but addr2line is
    # available in normal GCC/Clang development environments and is enough for
    # suppressions keyed to system functions such as pthread_once.
    "lsan": ["allow_addr2line=1"],
}
