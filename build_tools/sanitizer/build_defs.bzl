# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Dependency-owned sanitizer suppression metadata."""

load(":suppressions.bzl", "sanitizer_suppression_options")

IreeSanitizerSuppressionsInfo = provider(
    doc = "Suppression inputs declared by one runtime dependency.",
    fields = {"files": "Sanitizer names mapped to depsets of suppression files."},
)

def _suppression_set_impl(ctx):
    files = {}
    for sanitizer, target in zip(ctx.attr.kinds, ctx.attr.files):
        sanitizer_suppression_options(sanitizer, "")
        files.setdefault(sanitizer, []).extend(target[DefaultInfo].files.to_list())
    return [
        DefaultInfo(),
        IreeSanitizerSuppressionsInfo(files = {
            sanitizer: depset(inputs)
            for sanitizer, inputs in files.items()
        }),
    ]

_suppression_set = rule(
    implementation = _suppression_set_impl,
    attrs = {
        "files": attr.label_list(allow_files = True),
        "kinds": attr.string_list(),
    },
)

def declare_sanitizer_suppressions(name, data, suppressions, testonly = False):
    """Attaches dependency-owned sanitizer metadata to a target's runtime data.

    Args:
      name: Owning target name.
      data: Existing runtime data dependencies.
      suppressions: Sanitizer names mapped to suppression file labels.
      testonly: Whether the owning target is test-only.

    Returns:
      Runtime data dependencies including the declared policy, if any.
    """
    if not suppressions:
        return data
    metadata_name = name + "_sanitizer_suppressions"
    _suppression_set(
        name = metadata_name,
        files = [suppressions[kind] for kind in sorted(suppressions)],
        kinds = sorted(suppressions),
        testonly = testonly,
        visibility = ["//visibility:private"],
    )
    if data == None:
        data = []
    return data + [":" + metadata_name]
