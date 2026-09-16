# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Views of retained residency facts, without reevaluating a target model."""

from __future__ import annotations

from typing import cast

_SUMMARY_FIELDS = (
    "best_tier",
    "current_tier",
    "next_better_tier",
    "limiting_resource_count",
    "unavailable_reasons",
)
_RESOURCE_FIELDS = (
    "kind",
    "unit",
    "allocation_scope",
    "pool_scope",
    "pool_units",
    "allocation_granularity",
    "units",
    "rounded_units",
    "independent_tier",
    "limiting",
    "reduction_units_to_next_better_tier",
)
_UNAVAILABLE_REASONS = {
    "incomplete_resource_counts": "final resource counts are incomplete",
    "unknown_workgroup_size": "the fixed workgroup size is unavailable",
}


def residency_summary(entry: dict[str, object]) -> dict[str, object]:
    """Returns the input-validated summary, or no evidence before emission."""
    resources = cast(dict[str, object], entry.get("target_resources") or {})
    return cast(dict[str, object], resources.get("residency") or {})


def residency_transition_requirements(
    entry: dict[str, object],
    constraints: tuple[dict[str, object], ...],
) -> tuple[dict[str, object], ...]:
    """Returns all producer-supplied reductions, or no exact joint guidance."""
    summary = residency_summary(entry)
    current = cast(int | None, summary.get("current_tier"))
    next_tier = cast(int | None, summary.get("next_better_tier"))
    if current is None or next_tier is None or next_tier <= current:
        return ()
    limiting = tuple(row for row in constraints if row.get("limiting") is True)
    if not limiting or len(limiting) != summary.get("limiting_resource_count"):
        return ()
    if any("reduction_units_to_next_better_tier" not in row for row in limiting):
        return ()
    return limiting


def build_residency_show(
    entry: dict[str, object], constraints: tuple[dict[str, object], ...]
) -> dict[str, object] | None:
    """Keeps resource identity independent of report row ordering."""
    summary = residency_summary(entry)
    if not summary and not constraints:
        return None
    view: dict[str, object] = {
        "summary": {key: summary[key] for key in _SUMMARY_FIELDS if key in summary},
        "resources": {
            row["name"]: {key: row[key] for key in _RESOURCE_FIELDS if key in row}
            for row in constraints
        },
    }
    requirements = residency_transition_requirements(entry, constraints)
    if requirements:
        view["next_tier_requirements"] = {
            row["name"]: {
                "reduction": row["reduction_units_to_next_better_tier"],
                "maximum_units": cast(int, row["units"])
                - cast(int, row["reduction_units_to_next_better_tier"]),
                "unit": row["unit"],
                "allocation_scope": row["allocation_scope"],
            }
            for row in requirements
        }
    return view


def build_residency_diff(
    baseline: dict[str, object] | None, candidate: dict[str, object] | None
) -> dict[str, object] | None:
    """Compares retained facts by resource name, including availability changes."""
    if baseline == candidate:
        return None
    return {"baseline": baseline, "candidate": candidate}


def _summary_text(summary: dict[str, object]) -> str:
    reasons = cast(list[str], summary.get("unavailable_reasons", []))
    if reasons:
        return "exact residency unavailable: " + "; ".join(
            _UNAVAILABLE_REASONS.get(reason, reason) for reason in reasons
        )
    current = summary.get("current_tier")
    if current is None:
        return "exact residency unavailable: final model evidence was not captured"
    result = (
        f"modeled residency {current}/{summary.get('best_tier', '?')} subgroups/SIMD"
    )
    next_tier = summary.get("next_better_tier")
    if next_tier is not None:
        result += f"; next tier {next_tier}"
    elif current == summary.get("best_tier"):
        result += "; at the hardware wave ceiling"
    else:
        result += "; no higher tier by reducing footprints at this launch shape"
    return result


def _resource_text(resource: dict[str, object]) -> str:
    kind = resource["kind"]
    parts = []
    if kind == "fixed_limit":
        parts.append("fixed launch ceiling")
    else:
        unit = f"{resource['unit']}/{resource['allocation_scope']}"
        units = resource.get("units")
        if units is None:
            parts.append(f"final usage unavailable ({unit})")
        else:
            parts.append(
                f"{units:,} {unit}; rounded allocation {resource['rounded_units']:,}"
            )
        parts.append(
            f"granularity {resource['allocation_granularity']:,}; "
            f"pool {resource['pool_units']:,} "
            f"{resource['unit']}/{resource['pool_scope']}"
        )
    if kind == "unconstrained_resource":
        parts.append("no independent residency limit in this model")
    elif "independent_tier" in resource:
        parts.append(f"allows {resource['independent_tier']} subgroups/SIMD")
    else:
        parts.append("independent ceiling unavailable")
    relation = resource.get("limiting")
    if relation is True:
        parts.append("limiting")
    elif relation is False:
        parts.append("nonlimiting")
    else:
        parts.append("limiting relation unknown")
    return "; ".join(parts)


def append_residency_show_text(lines: list[str], view: dict[str, object]) -> None:
    """Explains limits and joint thresholds as model evidence, not speedups."""
    summary = cast(dict[str, object], view["summary"])
    lines.append("  Residency constraints (target model)")
    lines.append(f"    {_summary_text(summary)}")
    resources = cast(dict[str, dict[str, object]], view["resources"])
    for name, resource in resources.items():
        lines.append(f"    {name}: {_resource_text(resource)}")
    if not resources:
        lines.append("    per-resource constraints were not captured")
    requirements = cast(
        dict[str, dict[str, object]], view.get("next_tier_requirements", {})
    )
    if requirements:
        lines.append(
            f"    Reaching tier {summary['next_better_tier']} requires all of:"
        )
        for name, row in requirements.items():
            lines.append(
                f"      {name}: reduce by {row['reduction']:,} "
                f"{row['unit']}/{row['allocation_scope']} "
                f"to at most {row['maximum_units']:,}"
            )
        lines.append(
            "    Recompile and benchmark; higher modeled residency "
            "is not a throughput guarantee."
        )
    elif "next_better_tier" in summary:
        lines.append("    the complete joint reduction requirements are unavailable")


def append_residency_diff_text(lines: list[str], view: dict[str, object]) -> None:
    """Shows changed constraints without treating a resource-count delta as a win."""
    baseline = cast(dict[str, object], view["baseline"] or {})
    candidate = cast(dict[str, object], view["candidate"] or {})
    lines.append("  Residency constraints (target model)")
    before_summary = cast(dict[str, object], baseline.get("summary", {}))
    after_summary = cast(dict[str, object], candidate.get("summary", {}))
    if before_summary != after_summary:
        lines.append(f"    baseline: {_summary_text(before_summary)}")
        lines.append(f"    candidate: {_summary_text(after_summary)}")
    before_resources = cast(dict[str, dict[str, object]], baseline.get("resources", {}))
    after_resources = cast(dict[str, dict[str, object]], candidate.get("resources", {}))
    for name in dict.fromkeys((*before_resources, *after_resources)):
        before = before_resources.get(name)
        after = after_resources.get(name)
        if before == after:
            continue
        lines.append(f"    {name}:")
        for label, resource in (("baseline", before), ("candidate", after)):
            if resource is None:
                lines.append(f"      {label}: not reported")
                continue
            lines.append(f"      {label}: {_resource_text(resource)}")
            reduction = cast(
                int | None, resource.get("reduction_units_to_next_better_tier")
            )
            if reduction is not None:
                maximum = cast(int, resource["units"]) - reduction
                lines.append(
                    f"        next-tier requirement: reduce by {reduction:,} "
                    f"{resource['unit']}/{resource['allocation_scope']} "
                    f"to at most {maximum:,}"
                )
