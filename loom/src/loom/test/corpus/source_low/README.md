# Source-Low Corpus

This directory contains positive, target-reusable source programs for
source-to-low lowering coverage. A corpus case should be a program we want every
compatible target to accept, lower, and eventually execute or compare against an
oracle. Every backend participates through a `TEMPLATE` fixture with its own
target binding, RUN mode, and output assertions. A backend that does not yet
implement the required capability records the precise structured diagnostic in
its fixture. This keeps the positive source shared and makes newly supported
behavior visible when that diagnostic stops appearing. Architectural
non-applicability follows from the source/target contract, not an implementation
gap. Profile-specific instruction selection and pass-mode checks can use
separate fixtures consuming the same corpus program.

Shared analysis and report semantics have focused tests beside the owning
analysis or pass. The shared source-to-low tests use the backend-independent
test Low target for behavior such as loop execution counts and nested emission
accounting. Corpus programs here exercise backend capabilities such as memory
lowering or instruction selection; duplicating a shared analysis assertion
across target bindings adds no coverage of those semantics.

Do not put target-specific diagnostic or expected-failure cases here. Rejection
tests belong beside the target or pass that owns the diagnostic vocabulary, where
they can assert the exact structured error or remark without forcing every other
target to inherit unrelated failure semantics.
