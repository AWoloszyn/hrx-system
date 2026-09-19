# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Direct type bindings on region argument declarations."""

import pytest

from loom.builtin_types import ALL_BUILTIN_TYPES
from loom.dialect.func import ALL_FUNC_OPS
from loom.dialect.scf import ALL_SCF_OPS
from loom.dialect.test import ALL_TEST_OPS
from loom.format.bytecode.reader import read_module
from loom.format.bytecode.writer import write_module
from loom.format.text.parser import ParseError, Parser
from loom.format.text.printer import Printer
from loom.ir import F32


def _formats() -> tuple[Parser, Printer]:
    parser = Parser()
    printer = Printer()
    for format in (parser, printer):
        format.register_types(ALL_BUILTIN_TYPES)
        for operations in (ALL_FUNC_OPS, ALL_SCF_OPS, ALL_TEST_OPS):
            format.register_ops(operations)
    return parser, printer


@pytest.mark.parametrize("argument_form", ["capture", "explicit", "element"])
def test_region_argument_type_bindings(argument_form: str) -> None:
    parser, printer = _formats()
    bodies = {
        "capture": (
            "  %result = scf.for %iv = [%lower to %upper step %step]"
            "(%iter = %input : tile<[%extent]xf32, %layout>) "
            "-> (tile<[%extent]xf32, %layout>) {\n"
            "    scf.yield %iter : tile<[%extent]xf32, %layout>\n"
            "  }\n"
        ),
        "explicit": (
            "  test.block_args %input : tile<[%extent]xf32, %layout> "
            "do(%iter: tile<[%extent]xf32, %layout>) {\n"
            "    test.use %iter : tile<[%extent]xf32, %layout>\n"
            "    test.yield\n"
            "  }\n"
        ),
        "element": (
            "  %result = test.map(%iter = %input : tile<[%extent]xf32, %layout>) {\n"
            "    test.yield %iter : f32\n"
            "  } -> (tile<[%extent]xf32, %layout>)\n"
        ),
    }
    module = parser.parse(
        "func.def @f(%extent: index, %layout: encoding, "
        "%input: tile<[%extent]xf32, %layout>, "
        "%lower: index, %upper: index, %step: index) {\n"
        + bodies[argument_form]
        + "  test.use %input : tile<[%extent]xf32, %layout>\n"
        "  func.return\n}\n",
        verify=True,
    )
    text = printer.print_module(module)
    loaded_text = parser.parse(text, verify=True)
    assert printer.print_module(loaded_text) == text
    for candidate in (module, loaded_text, read_module(write_module(module))):
        entry = candidate.body.ops[0].regions[0].blocks[0]
        nested = entry.ops[0].regions[0].blocks[0]
        argument_id = nested.arg_ids[-1]
        argument = candidate.values[argument_id]
        if argument_form == "element":
            assert argument.type == F32
            assert argument.dim_bindings == {}
            assert argument.encoding_binding == -1
        else:
            assert argument.dim_bindings == {0: entry.arg_ids[0]}
            assert argument.encoding_binding == entry.arg_ids[1]
        assert nested.ops[0].operands[0] == argument_id
        assert entry.ops[1].operands[0] == entry.arg_ids[2]


def test_capture_binding_is_not_visible_after_its_region() -> None:
    parser, _ = _formats()
    with pytest.raises(ParseError, match="undefined SSA value '%iter'"):
        parser.parse(
            "func.def @f(%input: tile<4xf32>) {\n"
            "  %result = test.map(%iter = %input : tile<4xf32>) {\n"
            "    test.yield %iter : f32\n"
            "  } -> (tile<4xf32>)\n"
            "  test.use %iter : f32\n"
            "  func.return\n"
            "}\n"
        )
