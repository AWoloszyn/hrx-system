# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from copy import deepcopy

import pytest

from loom.reporting.compile_report import CompileReportError, parse_compile_report
from loom.reporting.compile_report_view import (
    build_compile_report_diff,
    build_compile_report_show,
    format_compile_report_diff_text,
    format_compile_report_show_text,
)


def _compile_report() -> dict[str, object]:
    # Retained final gfx1151 wave64 attention facts. The reader consumes these
    # facts; target arithmetic is covered by the C occupancy API tests.
    constraints = [
        {
            "name": "amdgpu.sgpr",
            "kind": "unconstrained_resource",
            "unit": "registers",
            "allocation_scope": "subgroup",
            "pool_scope": "SIMD",
            "pool_units": 800,
            "allocation_granularity": 106,
            "units": 36,
            "rounded_units": 106,
            "independent_tier": 16,
            "limiting": False,
        },
        {
            "name": "amdgpu.vgpr",
            "kind": "pooled_resource",
            "unit": "registers",
            "allocation_scope": "subgroup",
            "pool_scope": "SIMD",
            "pool_units": 768,
            "allocation_granularity": 12,
            "units": 88,
            "rounded_units": 96,
            "independent_tier": 8,
            "limiting": True,
            "reduction_units_to_next_better_tier": 4,
        },
        {
            "name": "amdgpu.lds",
            "kind": "pooled_resource",
            "unit": "bytes",
            "allocation_scope": "workgroup",
            "pool_scope": "occupancy domain",
            "pool_units": 131072,
            "allocation_granularity": 512,
            "units": 15616,
            "rounded_units": 15872,
            "independent_tier": 8,
            "limiting": True,
            "reduction_units_to_next_better_tier": 1280,
        },
        {
            "name": "amdgpu.workgroup_slots",
            "kind": "fixed_limit",
            "independent_tier": 16,
            "limiting": False,
        },
    ]
    for index, row in enumerate(constraints):
        row.update({"index": index, "function": "attention"})
    return {
        "kind": "loom.compile_report",
        "schema_version": 0,
        "mode": "summary",
        "status": {"code": 0, "name": "OK"},
        "target_family": "amdgpu",
        "target_key": "gfx1151",
        "entries": {
            "count": 1,
            "rows": [
                {
                    "index": 0,
                    "function": "attention",
                    "target_resources": {
                        "resident_subgroups_per_simd": 8,
                        "occupancy_percent": 50,
                        "limiting_resource": "amdgpu.vgpr",
                        "residency": {
                            "best_tier": 16,
                            "current_tier": 8,
                            "next_better_tier": 9,
                            "limiting_resource_count": 2,
                        },
                    },
                }
            ],
        },
        "residency_constraints": {"count": len(constraints), "rows": constraints},
    }


@pytest.mark.parametrize("mode", ["summary", "details"])
def test_show_explains_units_rounding_and_joint_requirements(mode: str) -> None:
    report = _compile_report()
    report["mode"] = mode
    view = build_compile_report_show(parse_compile_report(report))
    residency = view["entries"][0]["residency"]
    assert residency["next_tier_requirements"] == {
        "amdgpu.vgpr": {
            "reduction": 4,
            "maximum_units": 84,
            "unit": "registers",
            "allocation_scope": "subgroup",
        },
        "amdgpu.lds": {
            "reduction": 1280,
            "maximum_units": 14336,
            "unit": "bytes",
            "allocation_scope": "workgroup",
        },
    }
    text = format_compile_report_show_text(view)
    assert "modeled residency 8/16 subgroups/SIMD; next tier 9" in text
    assert "88 registers/subgroup; rounded allocation 96; granularity 12" in text
    assert (
        "pool 131,072 bytes/occupancy domain; allows 8 subgroups/SIMD; limiting" in text
    )
    assert "no independent residency limit in this model; nonlimiting" in text
    assert "fixed launch ceiling; allows 16 subgroups/SIMD; nonlimiting" in text
    assert "Reaching tier 9 requires all of:" in text
    assert "amdgpu.lds: reduce by 1,280 bytes/workgroup to at most 14,336" in text
    assert "not a throughput guarantee" in text


def test_diff_matches_resource_identity_not_row_order() -> None:
    baseline = _compile_report()
    candidate = deepcopy(baseline)
    rows = candidate["residency_constraints"]["rows"]
    rows.reverse()
    for index, row in enumerate(rows):
        row["index"] = index
    view = build_compile_report_diff(
        parse_compile_report(baseline), parse_compile_report(candidate)
    )
    assert view["entries"] == []
    assert view["unchanged_entry_count"] == 1


def test_diff_surfaces_constraint_change_without_scalar_metric_change() -> None:
    baseline = _compile_report()
    candidate = deepcopy(baseline)
    candidate["residency_constraints"]["rows"][2]["units"] = 15872
    candidate["residency_constraints"]["rows"][2][
        "reduction_units_to_next_better_tier"
    ] = 1536
    view = build_compile_report_diff(
        parse_compile_report(baseline), parse_compile_report(candidate)
    )
    assert len(view["entries"]) == 1
    residency = view["entries"][0]["residency"]
    assert (
        residency["baseline"]["next_tier_requirements"]["amdgpu.lds"]["reduction"]
        == 1280
    )
    assert (
        residency["candidate"]["next_tier_requirements"]["amdgpu.lds"]["reduction"]
        == 1536
    )
    text = format_compile_report_diff_text(view)
    assert "amdgpu.lds:" in text
    assert "baseline: 15,616 bytes/workgroup" in text
    assert "candidate: 15,872 bytes/workgroup" in text
    assert (
        "next-tier requirement: reduce by 1,280 bytes/workgroup to at most 14,336"
        in text
    )
    assert (
        "next-tier requirement: reduce by 1,536 bytes/workgroup to at most 14,336"
        in text
    )
    assert "amdgpu.vgpr:" not in text


def test_diff_reports_missing_and_added_inventory() -> None:
    complete = _compile_report()
    incomplete = deepcopy(complete)
    del incomplete["residency_constraints"]
    for before, after in ((complete, incomplete), (incomplete, complete)):
        view = build_compile_report_diff(
            parse_compile_report(before), parse_compile_report(after)
        )
        assert view["changed_entry_count"] == 1
        text = format_compile_report_diff_text(view)
        assert "not reported" in text
        assert "next-tier requirement: reduce by 1,280 bytes/workgroup" in text


def test_unknown_evidence_is_not_zero_or_exact_occupancy() -> None:
    report = _compile_report()
    resources = report["entries"]["rows"][0]["target_resources"]
    resources["residency"] = {
        "unavailable_reasons": ["incomplete_resource_counts", "unknown_workgroup_size"]
    }
    for row in report["residency_constraints"]["rows"]:
        row.pop("limiting", None)
        row.pop("reduction_units_to_next_better_tier", None)
    vector = report["residency_constraints"]["rows"][1]
    for field in ("units", "rounded_units", "independent_tier"):
        del vector[field]
    view = build_compile_report_show(parse_compile_report(report))
    analysis = view["entries"][0]["compiler_analysis"]
    assert "occupancy_percent" not in analysis
    assert "resident_subgroups_per_simd" not in analysis
    assert "limiting_resource" not in analysis
    assert "next_tier_requirements" not in view["entries"][0]["residency"]
    text = format_compile_report_show_text(view)
    assert (
        "final resource counts are incomplete; the fixed workgroup size is unavailable"
        in text
    )
    assert "final usage unavailable (registers/subgroup)" in text
    assert "independent ceiling unavailable; limiting relation unknown" in text
    assert "Reaching tier" not in text


def test_zero_usage_remains_known_and_fixed_ceiling_has_no_reduction() -> None:
    report = _compile_report()
    summary = report["entries"]["rows"][0]["target_resources"]["residency"]
    summary.pop("next_better_tier")
    summary["limiting_resource_count"] = 2
    rows = report["residency_constraints"]["rows"]
    rows[1].pop("reduction_units_to_next_better_tier")
    rows[2].update(
        {"units": 0, "rounded_units": 0, "independent_tier": 16, "limiting": False}
    )
    rows[2].pop("reduction_units_to_next_better_tier")
    rows[3].update({"independent_tier": 8, "limiting": True})
    view = build_compile_report_show(parse_compile_report(report))
    text = format_compile_report_show_text(view)
    assert "0 bytes/workgroup; rounded allocation 0" in text
    assert "fixed launch ceiling; allows 8 subgroups/SIMD; limiting" in text
    assert "no higher tier by reducing footprints at this launch shape" in text
    assert "next_tier_requirements" not in view["entries"][0]["residency"]


def test_multi_entry_inventory_is_indexed_by_emitted_function() -> None:
    report = _compile_report()
    other = deepcopy(report["entries"]["rows"][0])
    other.update({"index": 1, "function": "projection"})
    report["entries"]["rows"].append(other)
    report["entries"]["count"] = 2
    rows = report["residency_constraints"]["rows"]
    other_rows = deepcopy(rows)
    for index, row in enumerate(other_rows, start=len(rows)):
        row.update({"index": index, "function": "projection"})
    other_rows[0]["units"] = 40
    rows.extend(other_rows)
    report["residency_constraints"]["count"] = len(rows)
    document = parse_compile_report(report)
    assert set(document.residency_constraints_by_function) == {
        "attention",
        "projection",
    }
    view = build_compile_report_show(document)
    assert [
        entry["residency"]["resources"]["amdgpu.sgpr"]["units"]
        for entry in view["entries"]
    ] == [36, 40]


def test_missing_inventory_keeps_summary_without_fabricated_guidance() -> None:
    report = _compile_report()
    del report["residency_constraints"]
    text = format_compile_report_show_text(
        build_compile_report_show(parse_compile_report(report))
    )
    assert "modeled residency 8/16" in text
    assert "per-resource constraints were not captured" in text
    assert "complete joint reduction requirements are unavailable" in text


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("kind", "unknown", "invalid residency constraint kind"),
        ("unit", "", "nonempty string"),
        ("units", -1, "nonnegative integer"),
        ("units", True, "integer"),
        ("limiting", "yes", "boolean"),
        ("allocation_granularity", 0, "positive integer"),
        ("pool_units", -1, "nonnegative integer"),
        ("reduction_units_to_next_better_tier", 0, "known limiting footprint"),
        ("reduction_units_to_next_better_tier", 89, "known limiting footprint"),
    ],
)
def test_parser_rejects_malformed_resource_facts(
    field: str, value: object, message: str
) -> None:
    report = _compile_report()
    report["residency_constraints"]["rows"][1][field] = value
    with pytest.raises(CompileReportError, match=message):
        parse_compile_report(report)


@pytest.mark.parametrize(
    ("malformation", "message"),
    [
        ("count", "count is"),
        ("index", "expected 1"),
        ("duplicate", "unique nonempty"),
        ("missing_rounding", "available together"),
        ("fixed_footprint", "no reducible footprint"),
        ("nonlimiting_reduction", "known limiting footprint"),
        ("unknown_transition", "cannot claim exact transitions"),
        ("boolean_tier", "integer"),
        ("empty_reason", "nonempty reason strings"),
    ],
)
def test_parser_rejects_inconsistent_evidence(malformation: str, message: str) -> None:
    report = _compile_report()
    inventory = report["residency_constraints"]
    rows = inventory["rows"]
    summary = report["entries"]["rows"][0]["target_resources"]["residency"]
    if malformation == "count":
        inventory["count"] = 1
    elif malformation == "index":
        rows[1]["index"] = 0
    elif malformation == "duplicate":
        rows[1]["name"] = rows[0]["name"]
    elif malformation == "missing_rounding":
        del rows[1]["rounded_units"]
    elif malformation == "fixed_footprint":
        rows[3]["units"] = 1
    elif malformation == "nonlimiting_reduction":
        rows[1]["limiting"] = False
    elif malformation == "unknown_transition":
        summary["unavailable_reasons"] = ["incomplete_resource_counts"]
    elif malformation == "boolean_tier":
        summary["current_tier"] = True
    else:
        summary["unavailable_reasons"] = []
    with pytest.raises(CompileReportError, match=message):
        parse_compile_report(report)
