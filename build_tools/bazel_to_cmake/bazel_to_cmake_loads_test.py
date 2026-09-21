# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for shared declarative module loading, independent of project rules."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import bazel_to_cmake_loads


class ModuleLoaderTest(unittest.TestCase):
    def setUp(self):
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        self.root = Path(temporary_directory.name)
        self.loader = bazel_to_cmake_loads.ModuleLoader(
            self.root, bindings={"policy": dict}, repo_map={"@workspace": ""}
        )

    def write_module(self, path, source):
        module = self.root / path
        module.parent.mkdir(parents=True, exist_ok=True)
        module.write_text(source, encoding="utf-8")

    def test_nested_imports_aliases_and_helpers_share_module_exports(self):
        self.write_module("settings/defaults.bzl", 'OPTIONS = ["shared"]\n')
        self.write_module(
            "settings/profiles.bzl",
            """
load(":defaults.bzl", SHARED_OPTIONS = "OPTIONS")
load("//rules:defs.bzl", make_policy = "policy")

def with_options(name):
    return make_policy(name = name, options = SHARED_OPTIONS + [name])

PROFILE = with_options("configured")
""",
        )
        self.write_module(
            "consumer/exports.bzl",
            'load("@workspace//settings:profiles.bzl", EXPORTED = "PROFILE")\n',
        )
        exported = self.loader.symbol("//consumer:exports.bzl", "EXPORTED", self.root)
        direct = self.loader.symbol("//settings:profiles.bzl", "PROFILE", self.root)
        self.assertEqual(
            exported, {"name": "configured", "options": ["shared", "configured"]}
        )
        self.assertIs(exported, direct)

    def test_repository_root_imports_resolve_in_the_checkout(self):
        self.write_module("defaults.bzl", 'VALUE = ["root"]\n')
        self.write_module(
            "consumer/exports.bzl",
            'load("//:defaults.bzl", "VALUE")\n',
        )
        exported = self.loader.symbol("//consumer:exports.bzl", "VALUE", self.root)
        direct = self.loader.symbol("@workspace//:defaults.bzl", "VALUE", self.root)
        self.assertEqual(exported, ["root"])
        self.assertIs(exported, direct)

    def test_failed_evaluation_never_exports_partial_values(self):
        self.write_module(
            "settings/broken.bzl", 'VALUE = ["incomplete"]\nunsupported_starlark()\n'
        )
        for name in ("VALUE", "MISSING"):
            with self.subTest(name=name):
                value = self.loader.symbol("//settings:broken.bzl", name, self.root)
                with self.assertRaisesRegex(
                    NotImplementedError, "broken.bzl.*NameError.*unsupported_starlark"
                ):
                    bool(value)

    def test_unconsumed_rule_import_does_not_hide_declarative_exports(self):
        self.write_module("rules/defs.bzl", "BUILD_RULE = provider()\n")
        self.write_module(
            "settings/values.bzl",
            'load("//rules:defs.bzl", "BUILD_RULE")\nVALUE = "complete"\n',
        )
        self.assertEqual(
            self.loader.symbol("//settings:values.bzl", "VALUE", self.root),
            "complete",
        )
        rule = self.loader.symbol("//settings:values.bzl", "BUILD_RULE", self.root)
        with self.assertRaisesRegex(NotImplementedError, "NameError.*provider"):
            rule()

    def test_import_cycles_fail_even_when_values_are_overwritten(self):
        self.write_module(
            "settings/first.bzl",
            'load(":second.bzl", "VALUE")\nVALUE = "first"\n',
        )
        self.write_module(
            "settings/second.bzl",
            'load(":first.bzl", "VALUE")\nVALUE = "second"\n',
        )
        value = self.loader.symbol("//settings:first.bzl", "VALUE", self.root)
        with self.assertRaisesRegex(NotImplementedError, "cyclic .bzl load"):
            str(value)

    def test_missing_imports_retain_the_failure_context(self):
        self.write_module("settings/values.bzl", "PRESENT = True\n")
        for label, message in (
            ("//settings:missing.bzl", "FileNotFoundError"),
            ("//settings:values.bzl", "does not export 'MISSING'"),
            ("@external//settings:values.bzl", "not a local repository module"),
        ):
            with self.subTest(label=label):
                value = self.loader.symbol(label, "MISSING", self.root)
                with self.assertRaisesRegex(NotImplementedError, message):
                    value()


if __name__ == "__main__":
    unittest.main()
