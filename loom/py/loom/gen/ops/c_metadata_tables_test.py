# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Representation limits and cross-field contracts for C metadata tables."""

import pytest

from loom.assembly import AssemblyFormat, BlockArgs, Region
from loom.dsl import ANY, ATTR_TYPE_I64, ATTR_TYPE_SYMBOL, INTEGER, SYMBOL_DEFINE, AttrDef, Dialect, EnumCase, EnumDef, Op, Operand, RegionDef, Result, SameType, SymbolDefinition, SymbolValueContract
from loom.gen.ops.c_metadata_tables import generate_tables_c


def test_generate_tables_rejects_variadic_symbol_value_contract_result() -> None:
    op = Op(
        "test.value",
        group=Dialect("test"),
        traits=[SYMBOL_DEFINE],
        attrs=[AttrDef("name", ATTR_TYPE_SYMBOL)],
        results=[Result("types", ANY, variadic=True)],
        symbol_def=SymbolDefinition(
            field="name",
            name="test value",
            interfaces=["record"],
            value_contract=SymbolValueContract(result="types"),
        ),
    )

    with pytest.raises(ValueError, match="value contract result 'types' must not be variadic"):
        generate_tables_c("test", 0x01, [op])


def test_generate_tables_rejects_non_predicate_value_contract_attr() -> None:
    op = Op(
        "test.value",
        group=Dialect("test"),
        traits=[SYMBOL_DEFINE],
        attrs=[
            AttrDef("name", ATTR_TYPE_SYMBOL),
            AttrDef("predicates", ATTR_TYPE_I64),
        ],
        results=[Result("type", ANY)],
        symbol_def=SymbolDefinition(
            field="name",
            name="test value",
            interfaces=["record"],
            value_contract=SymbolValueContract(result="type", predicates="predicates"),
        ),
    )

    with pytest.raises(ValueError, match="predicates 'predicates' must name a predicate_list"):
        generate_tables_c("test", 0x01, [op])


def test_rejects_duplicate_assembly_mnemonics() -> None:
    dialect = Dialect("test")
    ops = [Op(f"test.{name}", group=dialect, assembly=AssemblyFormat("copy")) for name in ("first", "second")]
    with pytest.raises(ValueError, match="duplicate assembly mnemonic"):
        generate_tables_c("test", 0x01, ops)


def test_assembly_format_preserves_region_signature_ownership() -> None:
    op = Op(
        "test.region",
        group=Dialect("test"),
        regions=[RegionDef("body")],
        format=[BlockArgs("body"), Region("body")],
        assembly=AssemblyFormat("region", [Region("body")]),
    )
    with pytest.raises(ValueError, match="must preserve region argument ownership"):
        generate_tables_c("test", 0x01, [op])


def test_generate_tables_rejects_constraint_field_index_above_6_bit_max() -> None:
    op = Op(
        "test.wide",
        group=Dialect("test"),
        operands=[Operand(f"input_{i}", INTEGER) for i in range(65)],
        constraints=[SameType("input_0", "input_64")],
    )

    with pytest.raises(
        ValueError,
        match=r"Op 'test\.wide' constraint SameType: field 'input_64' "
        r"index 64 exceeds LOOM_FIELD_REF 6-bit max 63",
    ):
        generate_tables_c("test", 0, [op])


def test_generate_tables_rejects_unknown_region_argument_uniform_scope() -> None:
    op = Op(
        "test.bad_region_scope",
        group=Dialect("test"),
        regions=[RegionDef("body", arg_uniform_scope="device")],
        format=[Region("body")],
    )

    with pytest.raises(
        ValueError,
        match=r"Op 'test\.bad_region_scope' region 'body' has unsupported "
        r"arg_uniform_scope 'device'",
    ):
        generate_tables_c("test", 0, [op])


def test_constraint_count_fits_vtable_storage() -> None:
    for count in (255, 256):
        op = Op(
            "test.constraints",
            group=Dialect("test"),
            operands=[Operand("input", ANY)],
            constraints=[SameType("input")] * count,
        )
        if count == 255:
            generate_tables_c("test", 0, [op])
        else:
            with pytest.raises(ValueError, match="constraint count exceeds uint8_t capacity"):
                generate_tables_c("test", 0, [op])


def test_external_enum_conflicting_names_rejected() -> None:
    dialect = Dialect("test")
    ops = [
        Op(
            f"test.mode{value}",
            group=dialect,
            attrs=[
                AttrDef(
                    "mode",
                    "enum",
                    enum_def=EnumDef(
                        "Mode",
                        [EnumCase(keyword, value)],
                        c_type="loom_shared_mode_t",
                        c_const_prefix="LOOM_SHARED_MODE",
                    ),
                )
            ],
        )
        for value, keyword in enumerate(("fast", "slow"))
    ]
    with pytest.raises(ValueError, match="conflicting case sets"):
        generate_tables_c("test", 0, ops)
