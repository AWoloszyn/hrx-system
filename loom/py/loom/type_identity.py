# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Invocation-local structural identity for immutable type/attribute graphs."""

from collections.abc import Mapping
from typing import Any

from loom.ir import (
    CanonicalAttrDict,
    DialectType,
    EncodingInstance,
    FunctionType,
    ParameterizedAttr,
    ParameterizedAttrArray,
    ParameterizedType,
    Predicate,
    PredicateListAttr,
    RegisterType,
    ScalarType,
    ShapedType,
)


def _parts(value: Any) -> tuple[tuple[Any, ...], tuple[Any, ...]]:
    """Separate immediate identity from graph edges, without recursive hashing."""
    match value:
        case ScalarType():
            return (None, value), ()
        case ShapedType(
            type_kind=kind,
            element_type=element,
            dims=dims,
            encoding=encoding,
            alignment=alignment,
        ):
            return (ShapedType, kind, element, alignment), (*dims, encoding)
        case FunctionType(arg_types=args, result_types=results):
            return (FunctionType, len(args)), (*args, *results)
        case DialectType(name=name, params=parameters):
            return (DialectType, name), parameters
        case RegisterType():
            return (
                RegisterType,
                value.descriptor_set_stable_id,
                value.register_class_id,
                value.unit_count,
                value.name,
            ), (value.value_type,)
        case ParameterizedType() | ParameterizedAttr():
            return (type(value), value.family_name), value.slots
        case EncodingInstance(name=name, params=parameters):
            return (EncodingInstance, name, tuple(key for key, _ in parameters)), tuple(
                child for _, child in parameters
            )
        case ParameterizedAttrArray(values=values) | PredicateListAttr(values=values):
            return (type(value),), values
        case Predicate(kind=kind, args=args):
            return (Predicate, kind), args
        case Mapping():
            keys = (
                tuple(value)
                if isinstance(value, CanonicalAttrDict)
                else tuple(sorted(value))
            )
            return (Mapping, keys), tuple(value[key] for key in keys)
        case list() | tuple():
            return (type(value),), tuple(value)
        case _:
            # Remaining scalar types and attributes have no structural children.
            return (None, value), ()


class TypeIdentity:
    """Exact structural IDs valid for one immutable comparison or codec invocation.

    Each key contains only a node's immediate label and completed child IDs.
    Sharing patterns need not match: equal independently built DAGs receive the
    same ID without expanding their paths. Work and temporary storage scale with
    reached nodes and edges, not their tree expansion; noncanonical mappings
    additionally sort their keys. No state is added to IR.
    """

    def __init__(self) -> None:
        # Source references keep Python object identities alive until completion.
        self._completed: dict[int, tuple[Any, int]] = {}
        # Immediate keys own canonical IDs, independently of source sharing.
        self._keys: dict[tuple[Any, ...], int] = {}

    def intern(self, root: Any) -> int:
        """Return the canonical ID, completing each newly reached node once."""
        completed = self._completed.get(id(root))
        if completed is not None:
            return completed[1]
        pending = [(root, False)]
        while pending:
            value, expanded = pending.pop()
            identity = id(value)
            if identity in self._completed:
                continue
            label, children = _parts(value)
            if children and not expanded:
                pending.append((value, True))
                pending.extend((child, False) for child in children)
                continue
            key = (
                label,
                tuple(self._completed[id(child)][1] for child in children)
                if children
                else (),
            )
            ordinal = self._keys.setdefault(key, len(self._keys))
            self._completed[identity] = (value, ordinal)
        return self._completed[id(root)][1]

    def equal(self, left: Any, right: Any) -> bool:
        """Compare complete values, sharing completed facts across a batch."""
        return left is right or self.intern(left) == self.intern(right)
