# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Region-local block identity and textual labels."""

from collections.abc import Sequence

from loom.format.text.tokenizer import ParseError, Token
from loom.ir import Block


class BlockScope:
    """Binds CFG labels to direct identities during one region parse.

    A branch may precede its destination header. The header fills the same
    Block object; no operation rewrite or second token pass is needed. Labels
    never resolve through a parent region, and this table contains no SSA names.
    """

    def __init__(self, filename: str) -> None:
        # Source identity used for duplicate and missing label diagnostics.
        self._filename = filename
        # One stable object per label encountered in this region.
        self._blocks: dict[str, Block] = {}
        # First reference tokens for labels whose headers have not been parsed.
        self._pending: dict[str, Token] = {}

    def reference(self, token: Token) -> Block:
        block = self._blocks.get(token.text)
        if block is None:
            block = Block(label=token.text)
            self._blocks[token.text] = block
            self._pending[token.text] = token
        return block

    def define(self, token: Token) -> Block:
        if token.text in self._blocks and token.text not in self._pending:
            raise ParseError(
                f"duplicate block label '^{token.text}'",
                token.location,
                self._filename,
            )
        block = self.reference(token)
        del self._pending[token.text]
        return block

    def finish(self) -> None:
        if self._pending:
            token = next(iter(self._pending.values()))
            raise ParseError(
                f"undefined block label '^{token.text}' in this region",
                token.location,
                self._filename,
            )


def plan_block_labels(
    blocks: Sequence[Block],
    *,
    entry_args_declared_by_parent: bool,
    referenced_blocks: Sequence[Block] = (),
) -> dict[int, str]:
    """Choose one spelling for each header and all edges targeting it."""
    targets = {id(block) for block in referenced_blocks} | {
        id(successor)
        for block in blocks
        for op in block.ops
        if not op.is_dead
        for successor in op.successors
    }
    reserved = {block.label for block in blocks if block.label}
    used: set[str] = set()
    labels: dict[int, str] = {}
    for index, block in enumerate(blocks):
        if not (
            index
            or block.label
            or id(block) in targets
            or (block.arg_ids and not entry_args_declared_by_parent)
        ):
            continue
        base = block.label or f"bb{index}"
        label = base
        if label in used or (not block.label and label in reserved):
            if base[0].isdigit():
                base = "$" + base
            suffix = index
            label = f"{base}${suffix}"
            while label in reserved or label in used:
                suffix += 1
                label = f"{base}${suffix}"
        labels[id(block)] = label
        used.add(label)
    return labels
