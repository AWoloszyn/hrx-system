# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Bazel composition for the public libamdf artifacts."""

load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_shared_library.bzl", "cc_shared_library")
load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//build_tools/bazel:cc.bzl", "iree_cc_library")
load("//build_tools/bazel:cc_opts.bzl", "cc_opts")
load("//build_tools/bazel:requirements.bzl", "apply_build_requirements")
load("//libamdf/requirements:defs.bzl", "LIBAMDF")
load(":cc.bzl", "amdf_cc_library")

def _amdf_static_link_impl(ctx):
    # cc_static_library folds object-bearing libraries into its archive but
    # exposes remaining dependencies and flags only as text output groups.
    # Keep native link requirements on the archive's own linker input: shared
    # library composition drops flag-only inputs that have no library attached.
    component_inputs = depset(
        transitive = [dep[CcInfo].linking_context.linker_inputs for dep in ctx.attr.components],
        order = "topological",
    )
    libraries = [
        library
        for linker_input in ctx.attr.archive[CcInfo].linking_context.linker_inputs.to_list()
        for library in linker_input.libraries
    ]
    user_link_flags = []
    additional_inputs = []
    for linker_input in component_inputs.to_list():
        libraries.extend([
            library
            for library in linker_input.libraries
            if not library.objects and not library.pic_objects
        ])
        user_link_flags.extend(linker_input.user_link_flags)
        additional_inputs.extend(linker_input.additional_inputs)
    return [CcInfo(linking_context = cc_common.create_linking_context(
        linker_inputs = depset([cc_common.create_linker_input(
            owner = ctx.label,
            libraries = depset(libraries),
            user_link_flags = depset(user_link_flags),
            additional_inputs = depset(additional_inputs),
        )]),
    ))]

_amdf_static_link = rule(
    implementation = _amdf_static_link_impl,
    attrs = {
        "archive": attr.label(providers = [CcInfo]),
        "components": attr.label_list(providers = [CcInfo]),
    },
    provides = [CcInfo],
)

def amdf_library(
        name,
        components,
        hdrs,
        win_def_file,
        runtime_data = None,
        target_compatible_with = None,
        visibility = None):
    """Composes package-owned components into the public libamdf ABI.

    Args:
      name: Public shared-library link target and artifact base name.
      components: Production library targets included in the distribution.
      hdrs: Public headers installed and exposed through `:headers`.
      win_def_file: Windows module-definition file naming exported symbols.
      runtime_data: Private runtime libraries required by the implementation.
      target_compatible_with: Constraints required by the composed provider.
      visibility: Bazel visibility shared by the public targets.
    """
    if visibility == None:
        visibility = ["//visibility:public"]
    runtime_data = runtime_data or []
    private_visibility = ["//libamdf:__pkg__"]
    policy = apply_build_requirements({}, [LIBAMDF])
    compatible_with = policy["target_compatible_with"] + (target_compatible_with or [])
    tags = policy["tags"]

    # Public headers remain analyzable when an implementation family is omitted.
    iree_cc_library(
        name = "headers",
        hdrs = hdrs,
        includes = ["include"],
        visibility = visibility,
    )
    native.filegroup(
        name = name + "_runtime",
        srcs = runtime_data,
        visibility = ["//libamdf:__subpackages__"],
    )

    # One archive owns the complete implementation. Native UMD entry points
    # and shared family code reference each other through private interfaces;
    # exposing their component archives would require caller link grouping.
    cc_static_library(
        name = name + "_static_artifact",
        # The archive checker rejects coalescible ASAN COMMON symbols. Actual
        # shared-library and static-consumer links enforce symbol resolution.
        features = ["-symbol_check"],
        tags = tags,
        target_compatible_with = compatible_with,
        visibility = visibility,
        deps = components,
    )
    cc_import(
        name = name + "_static_import",
        static_library = ":" + name + "_static_artifact",
        tags = tags,
        target_compatible_with = compatible_with,
        visibility = private_visibility,
    )
    _amdf_static_link(
        name = name + "_static_link",
        archive = ":" + name + "_static_import",
        components = components,
        tags = tags,
        target_compatible_with = compatible_with,
        visibility = private_visibility,
    )
    amdf_cc_library(
        name = name + "_static",
        data = runtime_data,
        target_compatible_with = target_compatible_with,
        visibility = visibility,
        deps = [":" + name + "_static_link"],
    )
    cc_shared_library(
        name = name + "_shared_artifact",
        # The shared artifact uses the same complete implementation archive.
        # As a direct root, cc_shared_library retains its public entry points
        # without exposing component archive ordering to the native linker.
        deps = [":" + name + "_static_link"],
        shared_lib_name = select({
            "@platforms//os:macos": "lib" + name + ".dylib",
            "@platforms//os:windows": name + ".dll",
            "//conditions:default": "lib" + name + ".so",
        }),
        tags = tags,
        target_compatible_with = compatible_with,
        user_link_flags = cc_opts.iree_code_link_options(),
        visibility = visibility,
        win_def_file = select({
            "@platforms//os:windows": win_def_file,
            "//conditions:default": None,
        }),
    )
    native.filegroup(
        name = name + "_shared_library",
        srcs = [":" + name + "_shared_artifact"],
        output_group = "main_shared_library_output",
        tags = tags,
        target_compatible_with = compatible_with,
        visibility = private_visibility,
    )
    native.filegroup(
        name = name + "_interface_library",
        srcs = [":" + name + "_shared_artifact"],
        output_group = "interface_library",
        tags = tags,
        target_compatible_with = compatible_with,
        visibility = private_visibility,
    )
    cc_import(
        name = name + "_shared_import",
        interface_library = select({
            "@platforms//os:windows": ":" + name + "_interface_library",
            "//conditions:default": None,
        }),
        shared_library = ":" + name + "_shared_library",
        tags = tags,
        target_compatible_with = compatible_with,
        visibility = private_visibility,
    )
    amdf_cc_library(
        name = name,
        data = runtime_data,
        defines = ["AMDF_SHARED_LIBRARY=1"],
        target_compatible_with = target_compatible_with,
        visibility = visibility,
        deps = [":" + name + "_shared_import"],
    )
