# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""C++ kernel inputs with independent floating-point and exact integer references."""

import math
import random
import struct
import sys
from pathlib import Path

ELEMENTS = {"f16": ("e", "<f2", 2), "f32": ("f", "<f4", 4), "i32": ("i", "<i4", 4)}


def rounded(value, element):
    code, _, _ = ELEMENTS[element]
    return struct.unpack("<" + code, struct.pack("<" + code, value))[0]


def write_npy(path, values, element):
    code, description, _ = ELEMENTS[element]
    header = repr({"descr": description, "fortran_order": False, "shape": (len(values),)})
    header += " " * ((64 - (10 + len(header) + 1) % 64) % 64) + "\n"
    path.write_bytes(b"\x93NUMPY\x01\x00" + struct.pack("<H", len(header)) + header.encode("ascii") + struct.pack(f"<{len(values)}{code}", *values))


class Case:
    def __init__(self, directory, name, element, count):
        self.directory = directory
        self.name = name
        self.element = element
        self.count = count
        self.guard = "-123" if element == "i32" else "-123.0"
        self.lines = [f"check.case public @{name} {{"]
        self.lines.append(f"  %storage = check.generate.fill value({self.guard}) : tensor<{count + 32}x{element}>")
        _, _, width = ELEMENTS[element]
        self.lines.append(f"  %output = check.tensor.view %storage offset({16 * width}) : tensor<{count + 32}x{element}> -> tensor<{count}x{element}>")

    def array(self, name, values):
        filename = f"{self.name}_{name}.npy"
        write_npy(self.directory / filename, values, self.element)
        self.lines.append(f'  %{name} = check.file.read.npy path("{filename}") : tensor<{len(values)}x{self.element}>')

    def scalar(self, name, value, element):
        self.lines.append(f"  %{name} = check.literal value({value}) : {element}")

    def launch(self, kernel, arguments, types):
        self.lines.append(f"  kernel.launch @{kernel}({arguments}) : ({types})")

    def finish(self, expected, tolerance=None):
        self.array("expected", expected)
        if tolerance is None:
            self.lines.append(f"  check.expect.bitwise actual(%output) expected(%expected) : tensor<{self.count}x{self.element}>")
        else:
            self.lines.append(f"  check.expect.close actual(%output) expected(%expected) atol({tolerance}) rtol({tolerance}) nan(same) : tensor<{self.count}x{self.element}>")
        _, _, width = ELEMENTS[self.element]
        for name, offset in [("prefix", 0), ("suffix", (self.count + 16) * width)]:
            self.lines.append(f"  %{name} = check.tensor.view %storage offset({offset}) : tensor<{self.count + 32}x{self.element}> -> tensor<16x{self.element}>")
        self.lines.append(f"  %guard = check.generate.fill value({self.guard}) : tensor<16x{self.element}>")
        for name in ["prefix", "suffix"]:
            self.lines.append(f"  check.expect.bitwise actual(%{name}) expected(%guard) : tensor<16x{self.element}>")
        self.lines.extend(["  check.return", "}", ""])
        return "\n".join(self.lines)


def attention(directory):
    cases = []
    for queries, keys, magnitude in [(1, 1, 1), (3, 17, 1), (5, 33, 16)]:
        rng = random.Random(730 + queries + keys)
        query = [rounded(rng.uniform(-magnitude, magnitude), "f32") for _ in range(queries * 64)]
        key = [rounded(rng.uniform(-magnitude, magnitude), "f32") for _ in range(keys * 64)]
        value = [rounded(rng.uniform(-2, 2), "f32") for _ in range(keys * 64)]
        expected = []
        for row in range(queries):
            scores = [math.fsum(query[row * 64 + channel] * key[item * 64 + channel] for channel in range(64)) * 0.125 for item in range(keys)]
            maximum = max(scores)
            weights = [math.exp(score - maximum) for score in scores]
            denominator = math.fsum(weights)
            expected.extend(math.fsum(weights[item] * value[item * 64 + channel] for item in range(keys)) / denominator for channel in range(64))
        case = Case(directory, f"attention_{queries}_{keys}_{magnitude}", "f32", queries * 64)
        for name, values in [("query", query), ("key", key), ("value", value)]:
            case.array(name, values)
        case.scalar("queries", queries, "i32")
        case.scalar("keys", keys, "i32")
        case.launch(
            "flash_attention", "%query, %key, %value, %output, %queries, %keys", f"tensor<{len(query)}xf32>, tensor<{len(key)}xf32>, tensor<{len(value)}xf32>, tensor<{len(expected)}xf32>, i32, i32"
        )
        cases.append(case.finish(expected, 0.0003))
    return "kernel.decl @flash_attention() launch(%query: buffer, %key: buffer, %value: buffer, %output: buffer, %query_count: i32, %key_count: i32)\n\n" + "\n".join(cases)


def rms_norm(directory):
    cases = []
    for columns in [1, 33, 129]:
        rng = random.Random(810 + columns)
        values = [rounded(rng.uniform(-2, 2), "f32") for _ in range(3 * columns)]
        epsilon = rounded(1e-5, "f32")
        expected = []
        for row in range(3):
            inputs = values[row * columns : (row + 1) * columns]
            scale = 1 / math.sqrt(math.fsum(value * value for value in inputs) / columns + epsilon)
            expected.extend(value * scale for value in inputs)
        case = Case(directory, f"rms_norm_{columns}", "f32", len(values))
        case.array("input", values)
        case.scalar("columns", columns, "i32")
        case.scalar("epsilon", epsilon, "f32")
        case.launch("llama_rms_norm", "%input, %output, %columns, %epsilon", f"tensor<{len(values)}xf32>, tensor<{len(values)}xf32>, i32, f32")
        cases.append(case.finish(expected, 0.0002))
    return "kernel.decl @llama_rms_norm() launch(%input: buffer, %output: buffer, %columns: i32, %epsilon: f32)\n\n" + "\n".join(cases)


def swiglu(directory):
    cases = []
    for columns in [1, 31, 65, 129]:
        rng = random.Random(920 + columns)
        values = [rounded(rng.uniform(-16, 16), "f16") for _ in range(6 * columns)]
        expected = []
        alpha = rounded(1.702, "f32")
        for row in range(3):
            for column in range(columns):
                gate = min(values[row * 2 * columns + column], 7)
                linear = min(max(values[row * 2 * columns + columns + column], -7), 7)
                expected.append(rounded(gate / (1 + math.exp(-alpha * gate)) * (linear + 1), "f16"))
        case = Case(directory, f"swiglu_f16_{columns}", "f16", len(expected))
        case.array("input", values)
        case.scalar("columns", columns, "i32")
        case.launch("aiter_swiglu_f16", "%input, %output, %columns", f"tensor<{len(values)}xf16>, tensor<{len(expected)}xf16>, i32")
        cases.append(case.finish(expected, 0.002))
    return "kernel.decl @aiter_swiglu_f16() launch(%input: buffer, %output: buffer, %columns: i32)\n\n" + "\n".join(cases)


def control_flow(directory):
    counts = [0, 1, 2, 7, 31, 64, 129]
    expected = []
    for count in counts:
        trips = max(1, count)
        inner = max(1, count & 3)
        nested = inner * count * (count - 1) // 2 + count * inner * (inner + 1) // 2
        expected.extend([count * (count + 1) // 2, count, trips * (trips + 1) // 2, trips, nested, count, trips, max(0, count - 1), trips, trips])
    case = Case(directory, "loop_semantics", "i32", len(expected))
    case.array("counts", counts)
    case.scalar("length", len(counts), "i32")
    case.launch("control_flow", "%counts, %output, %length", f"tensor<{len(counts)}xi32>, tensor<{len(expected)}xi32>, i32")
    return "kernel.decl @control_flow() launch(%counts: buffer, %output: buffer, %length: i32)\n\n" + case.finish(expected)


def scheduled_sum(directory):
    cases = []
    rows = 7
    for columns in [0, 1, 2, 5, 17, 33]:
        rng = random.Random(1030 + columns)
        values = [rng.randrange(-100, 101) for _ in range(rows * max(1, columns))]
        expected = [sum(values[row * columns : (row + 1) * columns]) for row in range(rows)]
        case = Case(directory, f"scheduled_sum_{columns}", "i32", rows)
        case.array("input", values)
        case.scalar("rows", rows, "i32")
        case.scalar("columns", columns, "i32")
        case.launch("scheduled_sum", "%input, %output, %rows, %columns", f"tensor<{len(values)}xi32>, tensor<{rows}xi32>, i32, i32")
        cases.append(case.finish(expected))
    return "kernel.decl @scheduled_sum() launch(%input: buffer, %output: buffer, %rows: i32, %columns: i32)\n\n" + "\n".join(cases)


def short_circuit(directory):
    cases = []
    for length in [0, 1, 17, 33, 64]:
        values = [((index * 7) % 9) - 3 for index in range(max(1, length))]
        expected = []
        for lane in range(64):
            present = lane < length
            truth = present and values[lane] != 0
            first = (lane & 1) != 0
            second = (lane & 2) != 0
            expected.extend(
                [
                    int(present),
                    int(truth),
                    2 * int(present),
                    int(not present or truth),
                    12 if first else 1,
                    int(first and second),
                    1 if first else 12,
                    int(first or second),
                    4 * int((lane & 8) != 0),
                    int(lane != 31 and lane != 32),
                ]
            )
        case = Case(directory, f"short_circuit_{length}", "i32", len(expected))
        case.array("input", values)
        case.scalar("length", length, "i32")
        case.launch("short_circuit", "%input, %output, %length", f"tensor<{len(values)}xi32>, tensor<{len(expected)}xi32>, i32")
        case.lines.append('  check.expect.event<device> {type = "asan_report", count = 0}')
        cases.append(case.finish(expected))
    return "kernel.decl @short_circuit() launch(%input: buffer, %output: buffer, %length: i32)\n\n" + "\n".join(cases)


def early_returns(directory):
    cases = []
    for length in [0, 1, 17, 33, 64]:
        values = [((index * 7) % 9) - 3 for index in range(max(1, length))]
        expected = [-123] * (64 * 6)
        for lane in range(length):
            value = values[lane]
            next_value = values[lane + 1] if lane + 1 < length else None
            classified = -7 if next_value is None else abs(next_value) if next_value < 0 else 3 if next_value == 0 else next_value + 4
            trace = 1 if next_value is None else 3 if next_value == 0 else 2
            total = sum(values[lane : min(lane + 3, length)]) - 7 * max(0, lane + 3 - length)
            chosen = (-11 if lane & 1 else -13) if value < 0 else (17 if lane & 1 else 19)
            published = 11 if value < 0 else value
            state = value + 1 if value < 0 else 5 if value == 0 else value + 7
            expected[lane * 6 : (lane + 1) * 6] = [classified, trace, total, chosen, published, state]
        case = Case(directory, f"early_returns_{length}", "i32", len(expected))
        case.array("input", values)
        case.scalar("length", length, "i32")
        case.launch("early_returns", "%input, %output, %length", f"tensor<{len(values)}xi32>, tensor<{len(expected)}xi32>, i32")
        case.lines.append('  check.expect.event<device> {type = "asan_report", count = 0}')
        cases.append(case.finish(expected))
    return "kernel.decl @early_returns() launch(%input: buffer, %output: buffer, %length: i32)\n\n" + "\n".join(cases)


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    for name, generator in [
        ("flash_attention", attention),
        ("llama_rms_norm", rms_norm),
        ("aiter_swiglu_f16", swiglu),
        ("control_flow", control_flow),
        ("scheduled_sum", scheduled_sum),
        ("short_circuit", short_circuit),
        ("early_returns", early_returns),
    ]:
        (directory / f"{name}.loom").write_text(generator(directory))


if __name__ == "__main__":
    main()
