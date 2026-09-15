# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Build settings with checked value domains."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _string_list_flag_impl(ctx):
    unknown_values = [
        value
        for value in ctx.build_setting_value
        if value not in ctx.attr.values
    ]
    if unknown_values:
        fail("Unknown value(s) for {}: {}. Expected values from: {}.".format(
            ctx.label,
            ", ".join(unknown_values),
            ", ".join(ctx.attr.values),
        ))
    return BuildSettingInfo(value = ctx.build_setting_value)

iree_string_list_flag = rule(
    implementation = _string_list_flag_impl,
    build_setting = config.string_list(flag = True),
    attrs = {
        "values": attr.string_list(
            mandatory = True,
            doc = "Allowed values. The empty selection is always valid.",
        ),
    },
    doc = "A command-line string-list setting restricted to declared values.",
)
