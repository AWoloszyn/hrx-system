# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Schema bindings for fixed positions in canonical Low dictionaries."""

from __future__ import annotations

from collections.abc import Iterable

from loom.gen.target.low import c_spelling
from loom.target.low_descriptors import Descriptor, ImmediateFlag, target_relative_name


def _descriptor_attr_indices(descriptor: Descriptor) -> dict[tuple[str, bool], int]:
    indices: dict[tuple[str, bool], int] = {}
    for index, immediate in enumerate(sorted(descriptor.immediates, key=lambda value: value.field_name)):
        optional = ImmediateFlag.DEFAULT_VALUE in immediate.flags
        if optional and index + 1 != len(descriptor.immediates):
            # A later field makes presence at this index ambiguous. A trailing
            # optional field is present exactly when dictionary count > index.
            break
        indices[(immediate.field_name, optional)] = index
    return indices


def emit_attr_indices(
    c_enum_prefix: str,
    descriptors: Iterable[Descriptor],
    *,
    target_key: str | None,
) -> list[str]:
    """Emits dictionary indices shared by every variant of each descriptor.

    A target's common header can pass multiple variants of the same descriptor.
    Only fields with a fixed, identical position in every variant are exposed.
    A trailing optional field uses OPTIONAL_ATTR_INDEX. The caller establishes
    its presence by comparing the dictionary count with the index.
    """

    descriptor_indices: dict[str, dict[tuple[str, bool], int]] = {}
    for descriptor in descriptors:
        indices = _descriptor_attr_indices(descriptor)
        if descriptor.key in descriptor_indices:
            indices = {name: index for name, index in descriptor_indices[descriptor.key].items() if indices.get(name) == index}
        descriptor_indices[descriptor.key] = indices

    lines: list[str] = []
    constant_fields: dict[str, tuple[str, str]] = {}
    for key, indices in descriptor_indices.items():
        if not indices:
            continue
        descriptor_name = c_spelling.c_identifier(target_relative_name(target_key, key)).upper()
        lines.extend([f"// Canonical dictionary indices for {key}.", "enum {"])
        for (field_name, optional), index in indices.items():
            suffix = "OPTIONAL_ATTR_INDEX" if optional else "ATTR_INDEX"
            constant = f"{c_enum_prefix}_{descriptor_name}_{c_spelling.c_identifier(field_name).upper()}_{suffix}"
            if constant in constant_fields:
                previous_key, previous_field = constant_fields[constant]
                raise ValueError(f"attribute index '{constant}' collides between '{previous_key}.{previous_field}' and '{key}.{field_name}'")
            constant_fields[constant] = (key, field_name)
            lines.append(f"  {constant} = {index},")
        lines.extend(["};", ""])
    return lines
