# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Reads the toolchain and configuration recorded by CMake."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from build_tools.devtools import cmake_file_api

CACHE_ENTRY_PATTERN = re.compile(r"^([^:#=]+):([^=]+)=(.*)$")


@dataclass(frozen=True)
class CMakeCacheEntry:
    # CMake variable name.
    name: str
    # Cache type, such as BOOL, FILEPATH, or INTERNAL.
    type: str
    # Unevaluated value recorded in CMakeCache.txt.
    value: str


def load_cmake_cache(build_dir: Path) -> list[CMakeCacheEntry]:
    cache_path = build_dir / "CMakeCache.txt"
    try:
        lines = cache_path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError as exc:
        raise cmake_file_api.FileApiError(
            f"CMake cache is missing; run iree-cmake-configure first: {cache_path}"
        ) from exc
    entries = []
    for line in lines:
        match = CACHE_ENTRY_PATTERN.match(line)
        if match is None:
            continue
        name, entry_type, value = match.groups()
        entries.append(CMakeCacheEntry(name=name, type=entry_type, value=value))
    return entries
