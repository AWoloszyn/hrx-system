# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared implementation helpers for executable rules derived from rules_cc."""

load(
    ":execution_requirements.bzl",
    "collect_execution_requirements",
    "inject_execution_requirements",
)
load(":runfiles.bzl", "create_runfiles_arguments_info")

_RULES_CC_LINK_EXTRA_LIB = Label("@rules_cc//:link_extra_lib")

cc_execution_attrs = {
    "execution_data": attr.label_list(
        allow_files = True,
        aspects = [collect_execution_requirements],
        doc = "Internal mirror of data used to collect configured runtime requirements.",
    ),
    "execution_deps": attr.label_list(
        aspects = [collect_execution_requirements],
        doc = "Internal mirror of deps used to collect configured runtime requirements.",
    ),
}

def cc_execution_initializer(
        deps = None,
        data = None,
        linkshared = None,
        **_kwargs):
    """Preserves rules_cc macro policy and exposes graph edges to the aspect.

    Args:
      deps: C/C++ dependencies passed to the rules_cc macro surface.
      data: Runtime data passed to the rules_cc macro surface.
      linkshared: Whether the target is treated as a shared library.
      **_kwargs: Other inherited rules_cc attributes.

    Returns:
      Updated inherited attributes plus aspect-bearing dependency mirrors.
    """
    original_deps = deps
    if not linkshared:
        if deps == None:
            deps = [_RULES_CC_LINK_EXTRA_LIB]
        else:
            deps = deps + [_RULES_CC_LINK_EXTRA_LIB]
    return {
        "deps": deps,
        "execution_data": data,
        "execution_deps": original_deps,
    }

def cc_execution_impl(ctx):
    """Runs the rules_cc implementation and conditionally injects bindings.

    Args:
      ctx: Rule context inherited by the extended C/C++ rule.

    Returns:
      The complete parent provider list with configured runtime bindings.
    """
    providers = list(ctx.super())
    runfiles_arguments = create_runfiles_arguments_info(
        ctx,
        ctx.attr.data + ctx.attr.deps + ctx.attr.srcs,
    )
    if runfiles_arguments != None:
        providers.append(runfiles_arguments)
    dependencies = ctx.attr.execution_data + ctx.attr.execution_deps
    executable = None
    for value in providers:
        # rules_cc returns DebugPackageInfo as a Starlark provider. Its
        # unstripped file is the executable for both ordinary binaries and
        # linkshared outputs, where accessing ctx.outputs.executable is invalid.
        if hasattr(value, "target_label") and hasattr(value, "unstripped_file"):
            executable = value.unstripped_file
            break
    if executable == None:
        fail("%s rules_cc parent did not return DebugPackageInfo" % ctx.label)
    return inject_execution_requirements(
        ctx,
        providers,
        dependencies,
        executable,
    )
