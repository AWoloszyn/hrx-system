# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Analysis tests for compile command extraction rules."""

load("@rules_testing//lib:analysis_test.bzl", "analysis_test", "test_suite")
load("@rules_testing//lib:util.bzl", "util")
load("//build_tools/bazel:cc.bzl", "iree_cc_library")
load("//build_tools/bazel:compile_commands.bzl", "IreeCompileCommandsInfo", "iree_compile_commands")

def _contains(values, needle):
    for value in values:
        if needle in value:
            return True
    return False

def _test_compile_commands_collects_c_and_cxx_sources(name, **kwargs):
    util.helper_target(
        native.genrule,
        name = name + "_generated_source",
        outs = [name + "_generated.c"],
        cmd = "touch $@",
        tags = ["manual"],
    )
    util.helper_target(
        iree_cc_library,
        name = name + "_subject",
        conlyopts = ["-DC_ONLY_FLAG"],
        copts = ["-DCOMMON_FLAG"],
        cxxopts = ["-DCXX_ONLY_FLAG"],
        local_defines = ["LOCAL_DEFINE"],
        srcs = [
            ":" + name + "_generated_source",
            "compile_commands_fixture.c",
            "compile_commands_fixture.cc",
        ],
        tags = ["manual"],
    )
    util.helper_target(
        iree_compile_commands,
        name = name + "_commands",
        out = name + "_compile_commands.json",
        targets = [":" + name + "_subject"],
        tags = ["manual"],
    )
    analysis_test(
        name = name,
        attr_values = {
            "timeout": "short",
        },
        config_settings = {
            "//command_line_option:conlyopt": ["-DCONFIG_C_ONLY_FLAG"],
            "//command_line_option:copt": ["-DCONFIG_COMMON_FLAG"],
            "//command_line_option:cxxopt": ["-DCONFIG_CXX_ONLY_FLAG"],
        },
        impl = _test_compile_commands_collects_c_and_cxx_sources_impl,
        target = name + "_commands",
        **kwargs
    )

def _test_compile_commands_collects_c_and_cxx_sources_impl(env, target):
    entries = target[IreeCompileCommandsInfo].entries.to_list()
    if len(entries) != 2:
        env.fail("expected two compile command entries, got %d: %r" % (len(entries), entries))
    for expected in [
        "compile_commands_fixture.c",
        "compile_commands_fixture.cc",
        "LOCAL_DEFINE",
    ]:
        if not _contains(entries, expected):
            env.fail("expected %r in compile command entries: %r" % (expected, entries))
    for encoded_entry in entries:
        entry = json.decode(encoded_entry)
        arguments = entry["arguments"]
        is_c = entry["file"].endswith(".c")
        expected_flags = [
            "-DCONFIG_COMMON_FLAG",
            "-DCONFIG_C_ONLY_FLAG" if is_c else "-DCONFIG_CXX_ONLY_FLAG",
            "-DCOMMON_FLAG",
            "-DC_ONLY_FLAG" if is_c else "-DCXX_ONLY_FLAG",
        ]
        actual_flags = [argument for argument in arguments if argument in expected_flags]
        env.expect.that_collection(actual_flags).contains_exactly(expected_flags).in_order()
        excluded_flags = ["-DCONFIG_CXX_ONLY_FLAG", "-DCXX_ONLY_FLAG"] if is_c else ["-DCONFIG_C_ONLY_FLAG", "-DC_ONLY_FLAG"]
        for flag in excluded_flags:
            env.expect.that_collection(arguments).not_contains(flag)

def compile_commands_rules_test_suite(name):
    test_suite(
        name = name,
        tests = [
            _test_compile_commands_collects_c_and_cxx_sources,
        ],
    )
