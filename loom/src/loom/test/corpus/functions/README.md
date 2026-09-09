# Function correctness corpus

These programs exercise Loom's authored semantics through compiled execution.
Each file keeps ordinary functions together with their `check.case` inputs and
expected results. The sources have no target binding or target ABI: execution
configurations select a compiler target without changing the program or its
expectations. VM is the first function executor.

`BUILD.bazel` enrolls every `.loom` source in the suite. Additional execution
configurations consume the same sources. Device executors that require kernels
need function-to-kernel adaptation at their execution boundary, not rewritten
test bodies. Target-specific lowering expectations remain in the `source_low`
TEMPLATE corpus; VM instruction and format tests remain in the runtime.

## Coverage

The suite checks integer and floating arithmetic, exact bit operations,
conversion and rounding boundaries, control flow, calls, storage pressure,
buffer ownership, and typed memory access. Files are grouped by the semantic
contract under test rather than a target instruction or implementation file.

Inputs enter through function arguments so execution exercises the compiled
operations instead of only constant folding. Expectations use independently
derived values or precisely justified identities. Signed zeros, NaNs, narrow
formats, and packed fields use bitwise comparisons when their payload is part
of the contract. Approximate operations use their authored accuracy policy.
Loop and call cases retain observable results from before and after the
boundary, including aliases and values live across nested calls.

A new case adds an observable distinction: a boundary value, type combination,
alias relationship, control-flow path, or interaction between operations. The
number of passing samples alone does not establish complete language coverage.
Unsupported valid programs identify compiler or execution-adapter gaps; their
semantics and expected results remain shared.
