# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Resolves declarative exports from Python-compatible repository .bzl files.

Converters supply implementations of the constructors whose values they consume.
Other Starlark APIs remain unsupported; this is not a Bazel rule evaluator.
Unrepresented imports may reach ignored rules, but consuming them fails with the
original module/import context. Failed evaluation never publishes partial exports.
"""

from pathlib import Path


class _LoadCycleError(NotImplementedError):
    """A recursive module import, rather than an unrepresented Starlark value."""


class OpaqueLoadedSymbol:
    """An import without a representation in the consuming converter."""

    def __init__(self, label, name, reason):
        self._label = label
        self._name = name
        self._reason = reason

    def _unsupported(self):
        raise NotImplementedError(
            f"loaded symbol {self._name!r} from {self._label!r} "
            f"has no Bazel-to-CMake representation: {self._reason}"
        )

    def __bool__(self):
        self._unsupported()

    def __call__(self, *args, **kwargs):
        self._unsupported()

    def __str__(self):
        self._unsupported()

    def __repr__(self):
        return f"OpaqueLoadedSymbol({self._label!r}, {self._name!r})"


class ModuleLoader:
    """Loads module exports once per set of converter-owned constructors.

    Bindings name known declarative constructors, not BUILD rule handlers. Loaded
    modules have separate namespaces and resolve relative imports from their own
    package. Repository aliases mapped to the local repository share one cache.
    """

    def __init__(self, repo_root, *, bindings, repo_map=None):
        self._repo_root = Path(repo_root)
        self._bindings = dict(bindings)
        self._repo_map = dict(repo_map or {})
        self._modules = {}
        self._failures = {}
        self._loading = []

    def symbol(self, label, name, package_dir):
        if name in self._bindings:
            return self._bindings[name]
        path = self._module_path(label, package_dir)
        if path is None:
            return OpaqueLoadedSymbol(label, name, "not a local repository module")
        try:
            namespace = self.load_file(path)
            if name not in namespace:
                raise NotImplementedError(f"{path} does not export {name!r}")
        except Exception as error:
            if isinstance(error, _LoadCycleError) and self._loading:
                raise
            return OpaqueLoadedSymbol(label, name, str(error))
        return namespace[name]

    def _module_path(self, label, package_dir):
        if label.startswith("@"):
            repository, separator, remainder = label.partition("//")
            if not separator or self._repo_map.get(repository, repository):
                return None
            label = "//" + remainder
        if label.startswith("//"):
            package, _, name = label[2:].partition(":")
            return (self._repo_root / package / name).resolve()
        if label.startswith(":"):
            return (Path(package_dir) / label[1:]).resolve()
        return None

    def load_file(self, path):
        """Returns complete exports, raising if the module cannot be evaluated."""
        path = Path(path).resolve()
        if path in self._modules:
            return self._modules[path]
        if path in self._failures:
            raise NotImplementedError(self._failures[path])
        if path in self._loading:
            cycle = " -> ".join(str(entry) for entry in [*self._loading, path])
            raise _LoadCycleError(f"cyclic .bzl load: {cycle}")

        namespace = {"Label": str, **self._bindings}

        def load(label, *names, **aliases):
            imports = {name: name for name in names}
            imports.update(aliases)
            for local_name, exported_name in imports.items():
                namespace[local_name] = self.symbol(label, exported_name, path.parent)

        namespace["load"] = load
        self._loading.append(path)
        try:
            source = path.read_text(encoding="utf-8")
            exec(compile(source, str(path), "exec"), namespace)
        except _LoadCycleError:
            raise
        except Exception as error:
            reason = f"{path}: {type(error).__name__}: {error}"
            self._failures[path] = reason
            raise NotImplementedError(reason) from error
        finally:
            self._loading.pop()
        self._modules[path] = namespace
        return namespace
