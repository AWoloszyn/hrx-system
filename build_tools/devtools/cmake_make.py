# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Selected-root builds for CMake's Unix Makefiles generator."""

from __future__ import annotations

import shlex
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path


class MakeBuildError(ValueError):
    """The refreshed native graph does not describe a selected build root."""


def _selected_native_targets(
    build_dir: Path, build_targets: tuple[str, ...]
) -> list[str]:
    # CMake's convenience names enter /rule recipes, each of which starts a
    # separate Make traversal of its /all node. Those /all nodes own the full
    # dependency graph and the ordinary dependency-scanning/build recipes.
    selected_targets = set(build_targets)
    native_targets = {}
    defined_targets = set()
    with (build_dir / "CMakeFiles/Makefile2").open(encoding="utf-8") as makefile:
        for line in makefile:
            line = line.rstrip("\n")
            if line.startswith(".PHONY : ") and line.endswith("/all"):
                defined_targets.add(line.removeprefix(".PHONY : "))
            elif line.endswith("/rule"):
                target, separator, native_target = line.partition(": ")
                if separator and target in selected_targets:
                    native_targets[target] = (
                        native_target.removesuffix("/rule") + "/all"
                    )

    result = []
    for target in build_targets:
        native_target = native_targets.get(target)
        if native_target is None or native_target not in defined_targets:
            raise MakeBuildError(
                f"CMake Unix Makefiles graph has no native build entry for "
                f"selected target {target!r}"
            )
        # Retain the generator's Make escaping, including spaces and dollar
        # signs. These names become prerequisites, never shell arguments.
        result.append(native_target)
    return result


@contextmanager
def selected_build_arguments(
    build_dir: Path, build_targets: tuple[str, ...]
) -> Iterator[list[str]]:
    """Adds one aggregate over the selected roots in an already-refreshed graph.

    The ordinary cmake --build driver selects the make program and supplies
    parallelism and verbosity. A supplementary makefile starts one recursive
    traversal over CMake's own dependency graph, preserving its jobserver and
    every selected target's freshness checks. Only the root edges are temporary.
    """
    native_targets = _selected_native_targets(build_dir, build_targets)
    with tempfile.TemporaryDirectory(prefix="iree-ctest-") as temporary_dir:
        makefile = Path(temporary_dir) / "selected.make"
        # The recursive command crosses both Make and POSIX-shell quoting.
        makefile_argument = shlex.quote(makefile.as_posix()).replace("$", "$$")
        makefile.write_text(
            ".PHONY: iree-ctest-selected iree-ctest-selected-roots\n"
            "iree-ctest-selected:\n"
            "\t$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 -f "
            f"{makefile_argument} iree-ctest-selected-roots\n"
            + "".join(
                f"iree-ctest-selected-roots: {target}\n" for target in native_targets
            ),
            encoding="utf-8",
        )
        yield ["--target", "iree-ctest-selected", "--", "-f", str(makefile)]
