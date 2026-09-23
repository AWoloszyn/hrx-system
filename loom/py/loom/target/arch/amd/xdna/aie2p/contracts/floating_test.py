# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for AMD XDNA AIE2P native floating-point contracts."""

from loom.dialect.scalar import arithmetic as scalar
from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.floating import AIE2P_FLOATING_RULES
from loom.target.contracts import DescriptorResultType, Guard, Scalar, ValueRef, Vector


def test_bf16_vector_maximum_requires_nan_and_zero_permissions() -> None:
    for source_op in (vector.vector_maxnumf, vector.vector_maximumf):
        rules = [rule for rule in AIE2P_FLOATING_RULES if rule.source_op is source_op]
        assert len(rules) == 2
        for rule, lanes in zip(rules, (16, 32), strict=True):
            assert rule.guards == (
                Guard.value_type("lhs", Vector("bf16", lanes=lanes)),
                Guard.value_type("rhs", Vector("bf16", lanes=lanes)),
                Guard.value_type("result", Vector("bf16", lanes=lanes)),
                Guard.instance_flags_has_all("fastmath", "nnan"),
                Guard.instance_flags_has_all("fastmath", "nsz"),
            )
            assert len(rule.emit) == 1
            maximum = rule.emit[0]
            assert maximum.descriptor.key == "amd.xdna.aie2p.max.lt.bf16x32.native"
            assert maximum.operands == {
                "s1": ValueRef.operand("lhs"),
                "s2": ValueRef.operand("rhs"),
            }
            assert maximum.results == {
                "d": ValueRef.result("result"),
                "cmp": ValueRef.temporary("comparison"),
            }
            # The unused mask remains a real fixed-register descriptor result.
            assert maximum.result_types == {
                "d": DescriptorResultType(),
                "cmp": DescriptorResultType(),
            }


def test_bf16_scalar_maximum_broadcasts_and_extracts_raw_halfwords() -> None:
    for source_op in (scalar.scalar_maxnumf, scalar.scalar_maximumf):
        rule = next(
            rule for rule in AIE2P_FLOATING_RULES if rule.source_op is source_op
        )
        assert rule.guards == (
            Guard.value_type("lhs", Scalar("bf16")),
            Guard.value_type("rhs", Scalar("bf16")),
            Guard.value_type("result", Scalar("bf16")),
            Guard.instance_flags_has_all("fastmath", "nnan"),
            Guard.instance_flags_has_all("fastmath", "nsz"),
        )
        assert [emit.descriptor.key for emit in rule.emit] == [
            "amd.xdna.aie2p.splat.i16x32",
            "amd.xdna.aie2p.splat.i16x32",
            "amd.xdna.aie2p.max.lt.bf16x32.native",
            "amd.xdna.aie2p.extract.i16.immediate",
        ]
        assert rule.emit[0].operands == {"src": ValueRef.operand("lhs")}
        assert rule.emit[1].operands == {"src": ValueRef.operand("rhs")}
        assert rule.emit[2].result_types == {
            "d": DescriptorResultType(),
            "cmp": DescriptorResultType(),
        }
        assert rule.emit[3].immediates == {"idx": 0}
        assert rule.emit[3].results == {"dst": ValueRef.result("result")}
