# loom-check

`loom-check` is the golden-test runner for `.loom-test` files. It splits each
file into `// ====` cases, runs the selected `// RUN:` mode, and compares the
actual output or diagnostics against the inline expectation.

Optional source importers feed an ordinary Loom module into the same modes.
Enabling the [C++ importer](../../import/cxx/README.md#compiler-tests) adds
`.cxx-test` files, with source options selected by `// INPUT:`. The linked formats
are listed by `--list-input-formats`; `--input-format` selects one explicitly.

Roundtrip and pass modes print Low in assembly form by default, including when
an explicit `// ----` expectation is present. Creating or updating an expectation
does not change the output representation. `with-locations` retains that
assembly form while including source locations.

Roundtrip mode and successful pass IR output are reparsed and printed again
before comparison. An expectation can only be updated when the output parses
and its canonical text remains stable.

## Choosing The Test Boundary

C++ unit tests own API contracts and focused functionality. IR programs belong
in `.loom-test` files: parsing and printing examples, verification diagnostics,
pass transformations, and compiler analysis or allocation results. The fixture
lives beside the subsystem whose behavior it covers, with input and expected
output visible together. IR formatters and migration tools can then operate on
the program directly, and all such tests share the `loom-check` executable.

A C++ test acquiring `format/text:parser` just to set up an allocator or pass
has crossed that boundary. Embedding source strings, generating whole programs
in C++, or loading external IR into that unit executable all create the same
compiler-stack dependency. Parser API unit tests are different: parsing itself
is the behavior under test. Small valid analysis-data fixtures remain useful
for API boundary cases, paired with `.loom-test` coverage of the real producer.

Each fixture is listed in its owning `test/BUILD.bazel` using
`loom_check_test_suite`. The default runner, `loom-check-test`, includes the
target-neutral test dialect and `test.low.core` descriptor package. Optional
target-specific cases live with that target's tooling tests and runner. The
[test-file contract](../../testing/test_file.h) describes modes, annotations,
case separators, and expected output.

## Running And Updating Fixtures

### Focused Output Checks

When only a few properties matter, `with-checks` keeps assertions beside the IR
without pinning the rest of a report or transformed program:

```text
// RUN: with-checks compile-report source-to-low,low-dce
...
// ----
CHECK: COMPILE-REPORT: source_low_memory * unknown_dynamic_packets=0 * dynamic_write_bytes=20 *
CHECK: COMPILE-REPORT: source_low[*] * source_op=view.store * execution_count=5
CHECK-NOT: COMPILE-REPORT: source_low[*] * execution_count=0
```

Each `CHECK:` must match a whole output line after trimming outer whitespace;
`CHECK-NOT:` rejects any matching line. Checks are independent and unordered.
`*` matches any sequence within a line and `?` matches one character. Other
characters are literal. A terminal `count=5` cannot match `count=50`. Blank lines
and standalone `//` comments are ignored. At least one positive check is required;
empty patterns and unknown directives are errors.

This modifier works with every textual-output mode; `verify` uses diagnostic
annotations. Exact goldens remain useful for canonical formatting and complete
output contracts. Formatting preserves check text, and `--update` leaves both
passing and failing checks unchanged. Machine-readable update suggestions also
omit them: changing an assertion requires an intentional edit.

`compile-report <pipeline>` checks reports from source compilation passes.
For authored Low assembly, `emit low-compile-report @function` builds the shared
emission frame and prints its normal report summary, including static and dynamic
instruction counts. This uses the runner's linked target descriptors and the
same report collector as native emission.

### Source Target Lowering

`emit source-low` runs the shared source compilation pipeline. For a function
without an authored target, select its root and a linked target profile together:

```cpp
// RUN: emit source-low @entry target=vm:core output=low
unsigned entry(unsigned value) { return value + 1u; }
```

In a `.cxx-test`, import produces the source function and specialization supplies
its target facts, including reachable helpers. The same request works with
`.loom-test` source IR. `output=low` compares the resulting Low assembly;
`output=module` includes the rest of the module, and `output=none` checks only
source-located diagnostics. Functions with authored target bindings can use the
existing whole-module form without a function or target option. Pipeline-text
outputs describe the pipeline itself and do not accept specialization requests.

### Running Fixtures

Use checked-in Bazel test targets for normal verification:

```bash
iree-bazel-test --config=asan //loom/src/loom/tools/loom-check/test:test
iree-bazel-test --config=asan //loom/src/loom/...
```

To accept intentional output changes, pass the update flag through Bazel:

```bash
iree-bazel-test --config=asan <loom-check-test-target> --test_arg=--update
```

The `iree-bazel-test` wrapper detects `--test_arg=--update` and uses Bazel's
standalone TestRunner strategy so update-capable tests can rewrite checked-in
fixture files. Prefer that path over direct tool invocations when updating
repository tests.

Direct runs are useful for inspection:

```bash
iree-bazel-run //loom/src/loom/tools/loom-check -- path/to/file.loom-test
iree-bazel-run //loom/src/loom/tools/loom-check -- --update path/to/file.loom-test
```

`--update` cannot be used with stdin or verify-mode cases. Agent-oriented usage
is available from:

```bash
iree-bazel-run //loom/src/loom/tools/loom-check -- --agents_md
```

Emit-mode JSON targets should stay small. When a case only needs structured
diagnostics from the emitter, use `output=none` and check the `ERROR@` or
`REMARK@` annotations instead of checking in a large JSON blob.
