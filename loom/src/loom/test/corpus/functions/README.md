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

| Contract | Sources and important distinctions |
| --- | --- |
| Integer comparisons | `integer_comparison.loom`: every predicate at i32/i64, equality, both orders, signed/unsigned disagreement, high-bit boundaries. |
| Floating comparisons | `float_comparison.loom`: every predicate at f32/f64, adjacent values, signed zeros, subnormals, infinities, quiet/signaling NaNs in either operand. |
| Scalar values | `integer.loom`, `integer_boundaries.loom`, `floating.loom`, `division.loom`, `selection.loom`: arithmetic, wrapping overflow, endpoint shifts/rotates, division signs, unary operations, and selection. |
| Representation and accuracy | `bitfield.loom`, `conversion*.loom`, `narrow.loom`, `rounding.loom`, `float_selectors.loom`, `turns.loom`: widths, field boundaries, floating policies, and fused versus separate rounding. |
| Exponentials | `exponential.loom`: normal/subnormal powers of two, overflow/underflow, nonfinite inputs, and explicit approximate f32 policies. |
| Logarithms | `logarithm.loom`: base-2 and natural logs, subnormal normalization, neighbors of one, exceptional-value domains and independent vector lanes. |
| Square roots | `roots.loom`: normal/subnormal inputs, signed zero, negative values, infinities/NaNs, and selected-width rounding of square root followed by reciprocal. |
| Vector lane programs | `vector_fields.loom`: construction, signed and unsigned fields, full-width replacement, lane extraction, and typed stores through portable legalization. |
| Vector floating-point lanes | `vector_floating.loom`: independent lane rounding and sign operations, preserving signed zeros, subnormals, infinities and NaN payloads through scalarization. |
| Vector reductions | `vector_reductions.loom`: signed/unsigned extrema, wrapping sums, products, bitwise combining kinds, empty/nonzero initial values, transpose, and reductions over each matrix axis. |
| Vector recurrences | `vector_recurrence.loom`: freshly constructed loop state, zero-trip preservation, backedge transport, and wrapping lane arithmetic. |
| Control and ownership | `control.loom`, `structured_control.loom`, `calls.loom`, `direct_call.loom`, `spill.loom`, `buffer_calls.loom`: tuple joins, nested loops, while-condition effects, recursion, live values across calls, and returned aliases. |
| Host buffer calls | `buffer_arguments.loom`: owned results passed into later calls, returned aliases, interleaved scalar/reference results, mutation visibility, and empty allocations. |
| Memory | `address.loom`, `buffers.loom`, `buffer_access.loom`, `memory_boundaries.loom`, `view_access.loom`: offsets, partial patterns with canaries, same-root copies, unsigned byte ordering, guarded access, typed views, allocation lifetime, and vector value/mask/scatter-offset snapshots across aliasing writes and calls. |
| Masked memory | `masked_memory.loom`: inactive tails beyond a view, dynamic active lanes, untouched canaries, empty views, and compact expand/compress lane ordering. |
| Atomics | `atomics.loom`: scalar integer and floating reduction/RMW, returned-old values, successful and failed compare-exchange, dynamic subview addresses, and untouched storage. |

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
