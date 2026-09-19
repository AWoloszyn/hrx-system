# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Stable dominance ordering for serialized region bodies."""

from loom.ir import Block, Region


def ordered_blocks(region: Region) -> list[Block]:
    """Orders reachable dominators before their blocks, then unreachable blocks.

    Physical block order breaks ties. The module and its successor identities
    remain unchanged. Malformed CFGs retain their order for diagnostic printing.
    """
    blocks = region.blocks
    if len(blocks) <= 1:
        return blocks
    indices = {id(block): index for index, block in enumerate(blocks)}
    successors: list[list[int]] = [[] for _ in blocks]
    predecessors: list[list[int]] = [[] for _ in blocks]
    for index, block in enumerate(blocks):
        live_ops = [op for op in block.ops if not op.is_dead]
        for position, op in enumerate(live_ops):
            if op.successors and position != len(live_ops) - 1:
                return blocks
            for successor in op.successors:
                target = indices.get(id(successor))
                if target is None:
                    return blocks
                successors[index].append(target)
                predecessors[target].append(index)

    # Keep DFS on an explicit stack: authored CFG depth is not Python depth.
    visited = {0}
    pending = [(0, 0)]
    postorder: list[int] = []
    while pending:
        block, successor_index = pending[-1]
        if successor_index == len(successors[block]):
            postorder.append(block)
            pending.pop()
            continue
        pending[-1] = (block, successor_index + 1)
        successor = successors[block][successor_index]
        if successor not in visited:
            visited.add(successor)
            pending.append((successor, 0))
    reverse_postorder = list(reversed(postorder))
    ranks = {block: rank for rank, block in enumerate(reverse_postorder)}

    # Reference implementation of iterative immediate dominators. Only integer
    # graph facts participate after extraction; no operation is reexamined.
    dominators = {0: 0}
    changed = True
    while changed:
        changed = False
        for block in reverse_postorder[1:]:
            known = [pred for pred in predecessors[block] if pred in dominators]
            dominator = known[0]
            for predecessor in known[1:]:
                other = predecessor
                while dominator != other:
                    while ranks[dominator] > ranks[other]:
                        dominator = dominators[dominator]
                    while ranks[other] > ranks[dominator]:
                        other = dominators[other]
            if dominators.get(block) != dominator:
                dominators[block] = dominator
                changed = True

    order: list[int] = []
    emitted: set[int] = set()
    for block in range(len(blocks)):
        if block not in visited or block in emitted:
            continue
        ancestors: list[int] = []
        ancestor = block
        while ancestor not in emitted:
            ancestors.append(ancestor)
            if ancestor == 0:
                break
            ancestor = dominators[ancestor]
        for ancestor in reversed(ancestors):
            emitted.add(ancestor)
            order.append(ancestor)
    order.extend(block for block in range(len(blocks)) if block not in visited)
    return [blocks[index] for index in order]
