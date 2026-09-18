# loom-check

`loom-check` is the golden-test runner for `.loom-test` files. It splits each
file into `// ====` cases, runs the selected `// RUN:` mode, and compares the
actual output or diagnostics against the inline expectation.

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
