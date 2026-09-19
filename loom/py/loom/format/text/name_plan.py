# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Capture-safe display names for resolved module values."""

from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from loom.dsl import Op
from loom.fields import FieldLayout, resolve_fields
from loom.format.text.blocks import plan_block_labels
from loom.ir import (
    Block,
    Module,
    Operation,
    ParameterizedAttr,
    ParameterizedAttrArray,
    Predicate,
)


@dataclass(frozen=True)
class NamePlan:
    # Complete SSA spellings, including the sigil, indexed by module value ID.
    names: tuple[str, ...]
    # Values whose references require a name even in an optional definition.
    referenced: frozenset[int]
    # Required block headers and successor spellings, keyed by object identity.
    block_labels: dict[int, str]


@dataclass
class _Scope:
    # Definitions reserved together in one parser scope.
    definitions: list[int] = field(default_factory=list)
    # Resolved ordinary, type and attribute references emitted in this scope.
    references: list[int] = field(default_factory=list)
    # Independent child scopes, including symbol signatures and region bodies.
    children: list["_Scope"] = field(default_factory=list)


def _attribute_references(value: Any, references: list[int]) -> None:
    pending = [value]
    while pending:
        value = pending.pop()
        if isinstance(value, Predicate):
            references.extend(arg.value for arg in value.args if arg.tag == "value")
        elif isinstance(value, Mapping):
            pending.extend(value.values())
        elif isinstance(value, ParameterizedAttr):
            pending.extend(value.slots)
        elif isinstance(value, ParameterizedAttrArray):
            pending.extend(value.values)
        elif isinstance(value, list | tuple):
            pending.extend(value)


def _find_conflicts(
    root: _Scope, bare_names: list[str], spelling_owners: list[int]
) -> set[int]:
    active: dict[str, int] = {}
    renamed: set[int] = set()

    def check_scope(scope: _Scope) -> None:
        previous: dict[str, int | None] = {}
        local: dict[str, int] = {}
        for value_id in scope.definitions:
            owner = spelling_owners[value_id]
            name = bare_names[owner]
            if name in local and local[name] != owner:
                renamed.update((owner, local[name]))
            local[name] = owner
            if name not in previous:
                previous[name] = active.get(name)
            active[name] = owner
        for value_id in scope.references:
            owner = spelling_owners[value_id]
            binding = active.get(bare_names[owner])
            if binding is not None and binding != owner:
                renamed.add(owner)
        for child in scope.children:
            check_scope(child)
        for name, binding in previous.items():
            if binding is None:
                del active[name]
            else:
                active[name] = binding

    check_scope(root)
    return renamed


def plan_names(
    module: Module,
    declarations: Mapping[str, Op],
    layout: Callable[[Op], FieldLayout],
    operation: Operation | None = None,
) -> NamePlan:
    """Plan lexical spellings without changing IR or recovering IDs from names.

    Each parser scope reserves its definitions before checking references. A
    captured outer value and same-scope duplicate definitions receive suffixes;
    independent scopes and harmless shadowing keep their authored names. The
    scope graph is extracted once, then discarded after resolving spellings.
    """
    root = _Scope()
    seen_operations: set[int] = set()
    symbolic_scopes: list[_Scope] = []
    block_labels: dict[int, str] = {}
    # Projected region arguments have distinct identities but one printed
    # signature. Their spellings are owned by the signature argument.
    spelling_owners = list(range(len(module.values)))

    def type_references(value_id: int, scope: _Scope) -> None:
        value = module.values[value_id]
        scope.references.extend(value.dim_bindings.values())
        if value.encoding_binding >= 0:
            scope.references.append(value.encoding_binding)

    def collect_operation(op: Operation, enclosing: _Scope) -> None:
        if op.is_dead:
            return
        seen_operations.add(id(op))
        declaration = declarations.get(op.name)
        scope = enclosing
        if declaration is not None and any(
            trait.name == "SymbolDefine" for trait in declaration.traits
        ):
            scope = _Scope()
            enclosing.children.append(scope)
            if (
                declaration.symbol_def is not None
                and declaration.symbol_def.bytecode_kind == "LOOM_SYMBOL_GLOBAL"
            ):
                symbolic_scopes.append(scope)
            field_layout = layout(declaration)
            body_index = field_layout.func_body_region_index
            if body_index is None:
                arguments = op.operands
            elif body_index < len(op.regions) and op.regions[body_index].blocks:
                arguments = op.regions[body_index].blocks[0].arg_ids
            else:
                arguments = []
            scope.definitions.extend(arguments)
            scope.definitions.extend(op.results)
            for argument in arguments:
                type_references(argument, scope)
            fields = resolve_fields(field_layout, op, module)
            for definition in declaration.regions:
                if definition.arg_source is None:
                    continue
                for region in fields.regions(definition.name):
                    if not region.blocks:
                        continue
                    for projected, argument in zip(
                        region.blocks[0].arg_ids, arguments, strict=True
                    ):
                        spelling_owners[projected] = argument
        else:
            scope.definitions.extend(op.results)
        scope.references.extend(op.operands)
        for value_id in (*op.operands, *op.results):
            type_references(value_id, scope)
        _attribute_references(op.attributes, scope.references)
        declared_regions: set[int] = set()
        if declaration is not None:
            field_layout = layout(declaration)
            fields = resolve_fields(field_layout, op, module)
            declared_regions.update(
                id(region)
                for name in field_layout.entry_args_declared_by_parent
                for region in fields.regions(name)
            )
        for region in op.regions:
            child = _Scope()
            enclosing.children.append(child)
            collect_blocks(
                region.blocks,
                child,
                entry_args_declared_by_parent=id(region) in declared_regions,
            )

    def collect_blocks(
        blocks: Sequence[Block],
        scope: _Scope,
        *,
        entry_args_declared_by_parent: bool = False,
    ) -> None:
        block_labels.update(
            plan_block_labels(
                blocks, entry_args_declared_by_parent=entry_args_declared_by_parent
            )
        )
        for block in blocks:
            scope.definitions.extend(block.arg_ids)
            for argument in block.arg_ids:
                type_references(argument, scope)
            for op in block.ops:
                collect_operation(op, scope)

    collect_blocks([module.body], root)
    if operation is not None and id(operation) not in seen_operations:
        collect_operation(operation, root)
        detached_targets = list(
            {
                id(block): block
                for block in operation.successors
                if id(block) not in block_labels
            }.values()
        )
        block_labels.update(
            plan_block_labels(
                detached_targets,
                entry_args_declared_by_parent=False,
                referenced_blocks=detached_targets,
            )
        )

    defined: set[int] = set()
    referenced: set[int] = set()
    pending = [root]
    while pending:
        scope = pending.pop()
        defined.update(scope.definitions)
        referenced.update(scope.references)
        pending.extend(scope.children)
    # Global declaration types introduce local symbolic dimensions/encodings;
    # these are definitions even though they have no separate signature slot.
    implicit = referenced - defined
    for scope in symbolic_scopes:
        local = set(scope.references) & implicit
        scope.definitions.extend(sorted(local))
        defined.update(local)
    # Detached operations may reference values supplied by their caller. They
    # still need distinct spellings in a diagnostic fragment.
    root.definitions.extend(sorted(referenced - defined))

    bare_names = [value.name or str(index) for index, value in enumerate(module.values)]
    renamed = _find_conflicts(root, bare_names, spelling_owners)
    used_names = {
        bare_names[spelling_owners[value_id]] for value_id in defined | referenced
    }
    for owner in sorted(renamed):
        base = module.values[owner].name
        # A digit-only SSA spelling cannot be extended as an identifier.
        if base and base[0].isdigit():
            base = "$" + base
        suffix = 0
        candidate = f"{base}${owner}"
        while candidate in used_names:
            suffix += 1
            candidate = f"{base}${suffix}${owner}"
        bare_names[owner] = candidate
        used_names.add(candidate)
    referenced_owners = {spelling_owners[value_id] for value_id in referenced}
    return NamePlan(
        names=tuple("%" + bare_names[owner] for owner in spelling_owners),
        block_labels=block_labels,
        referenced=frozenset(
            value_id
            for value_id, owner in enumerate(spelling_owners)
            if owner in referenced_owners
        ),
    )
