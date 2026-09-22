# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact type/attribute identities without expanding shared paths."""

from loom.dialect.test import test_array_type, test_options_attr
from loom.ir import (
    F32,
    I32,
    CanonicalAttrDict,
    DialectType,
    DynamicDim,
    DynamicEncoding,
    EncodingInstance,
    FunctionType,
    ParameterizedAttrArray,
    PoolType,
    Predicate,
    PredicateArg,
    PredicateListAttr,
    RegisterType,
    ScalarType,
    ShapedType,
    StaticDim,
    TypeKind,
)
from loom.type_identity import TypeIdentity


def _identity_values() -> list[object]:
    vector = ShapedType(TypeKind.VECTOR, F32, (DynamicDim(1),))
    predicate = Predicate("eq", (PredicateArg("value", 1), PredicateArg("const", 2)))
    options = test_options_attr(mode="fast", element_type=vector)
    return [
        None,
        F32,
        I32,
        vector,
        ShapedType(TypeKind.VIEW, I32, (DynamicDim(1),)),
        ShapedType(TypeKind.VIEW, I32, (DynamicDim(1),), alignment=4),
        ShapedType(TypeKind.VIEW, I32, (DynamicDim(1),), alignment=2),
        ShapedType(TypeKind.VIEW, I32, (DynamicDim(1),), alignment=1),
        ShapedType(TypeKind.TILE, F32, (DynamicDim(1),)),
        ShapedType(TypeKind.VECTOR, F32, (DynamicDim(2),)),
        ShapedType(TypeKind.VECTOR, F32, (StaticDim(1),)),
        ShapedType(TypeKind.TILE, F32, (DynamicDim(1),), DynamicEncoding(2)),
        ShapedType(TypeKind.TILE, F32, (DynamicDim(1),), DynamicEncoding(3)),
        PoolType(DynamicDim(1)),
        FunctionType((vector, vector), (I32,)),
        FunctionType((vector,), (vector, I32)),
        DialectType("test.ref", (vector,)),
        DialectType("test.other", (vector,)),
        RegisterType(1, 2, 4, value_type=vector),
        RegisterType(1, 2, 8, value_type=vector),
        RegisterType(2, 2, 4, value_type=vector),
        RegisterType(1, 3, 4, value_type=vector),
        RegisterType(1, 2, 4, name="test.register", value_type=vector),
        RegisterType(1, 2, 4),
        test_array_type(element_type=vector),
        test_array_type(element_type=vector, metadata={}),
        test_array_type(element_type=vector, metadata={"shape": vector}),
        options,
        ParameterizedAttrArray((options, options)),
        ParameterizedAttrArray(),
        EncodingInstance("test.first", params=(("width", 4),)),
        EncodingInstance("test.first", alias="alias", params=(("width", 4),)),
        EncodingInstance("test.second", params=(("width", 4),)),
        predicate,
        PredicateListAttr((predicate,)),
        PredicateListAttr(),
        (),
        [],
        CanonicalAttrDict((("shape", vector), ("count", 4))),
        {"count": 4, "shape": vector},
    ]


def test_identities_match_structural_equality() -> None:
    values = _identity_values()
    identities = TypeIdentity()
    for left in values:
        for right in values:
            assert identities.equal(left, right) == (left == right)
    for left, right in zip(values, _identity_values(), strict=True):
        # Independent construction preserves canonical absent parameter slots.
        assert left == right
        assert identities.equal(left, right)


def test_scalar_identity_is_shared_between_roots_and_children() -> None:
    scalar = ScalarType(F32.kind)
    identities = TypeIdentity()
    root = FunctionType((scalar,), (scalar,))
    identities.intern(root)
    assert identities.intern(scalar) == identities.intern(F32)
    assert identities.equal(root, FunctionType((F32,), (F32,)))
    assert not identities.equal(scalar, I32)


def test_deep_shared_identity_is_iterative_and_sharing_independent() -> None:
    first = ShapedType(TypeKind.VECTOR, F32, (DynamicDim(1),))
    second = ShapedType(TypeKind.VECTOR, F32, (DynamicDim(1),))
    different = ShapedType(TypeKind.VECTOR, F32, (DynamicDim(2),))
    for _ in range(2048):
        first = FunctionType((first, first), (first,))
        second = FunctionType((second, second), (second,))
        different = FunctionType((different, different), (different,))
    identities = TypeIdentity()
    assert identities.equal(first, second)
    assert not identities.equal(first, different)
    assert identities.equal(
        FunctionType((first, first), ()), FunctionType((first, second), ())
    )
    assert identities.intern(first) == identities.intern(second)
