# C and C++ source import

The optional native importer translates a source translation unit into a
verified Loom module. It uses the standalone cxx parser and semantic frontend;
the importer has no LLVM or Python runtime dependency. The result is ordinary
editable High IR, retaining source names, locations, structured loops, shared
helpers, and concrete template instances.

Enable `--config=loom-importer-cxx` with Bazel or `-DLOOM_IMPORT_CXX=ON` with
CMake. The importer is disabled by default. Its C++23 and exception requirements
are confined to the frontend implementation.

```sh
iree-bazel-run --config=loom-importer-cxx \
  //loom/src/loom/tools/loom-import-cxx -- \
  --I=include --std=c++26 --root=attention attention.cpp
```

`loom-import-cxx` imports one translation unit, runs canonicalization, common
subexpression elimination and dead-code elimination, and prints Loom text.
`--cleanup=false` exposes the direct import. `--to=bc --output=module.loombc`
produces normal Loom bytecode for the existing compilation and linking tools.

Multiple kernels and ordinary functions can coexist. By default, concrete
definitions with external visibility are exported. Repeated `--root` options
select qualified source function names; their reachable helpers remain private.
Overloaded root names require disambiguation in the source. Template helpers
are instantiated by cxx before import. This interface does not yet define an
external C++ ABI for linking separately compiled C++ translation units.

Kernel launch contracts are explicit source attributes:

```cpp
[[loom::kernel, loom::workgroup_count(2, 1, 1),
  loom::workgroup_size(64, 1, 1)]]
void fill(float* output) {
  output[0u] = 42.0f;
}
```

Each exact launch attribute accepts three positive i32 integer constant
expressions. `loom::workgroup_count_range(xmin, xmax, ymin, ymax, zmin, zmax)`
and `loom::workgroup_size_range(...)` instead constrain required config values
with inclusive bounds. For example:

```cpp
[[loom::kernel, loom::workgroup_count_range(1, 4, 1, 1, 1, 1),
  loom::workgroup_size(64, 1, 1)]]
void configured(float* output) { output[0u] = 42.0f; }
```

The importer emits `config.decl @configured.workgroup_count.x` with a
`range(%value, 1, 4)` predicate. Normal Loom config specialization supplies
the exact value and rejects values outside the contract. An absent annotation
also produces three required configs, with no additional range constraint.
Config names use the imported kernel symbol, so overloads remain distinct.
Contracts on leading function declarations carry to the definition; conflicting
redeclarations and multiple contracts for the same dimension group are errors.
Import does not guess launch dimensions or select a physical target.

The native API in `import.h` accepts a finalized Loom context and arena block
pool and returns an owned module. Source rejection produces structured
diagnostics and a null module; infrastructure failures return status. The API
runs verification and leaves cleanup policy to its caller. Source buffers,
headers, and the frontend AST can be released as soon as import returns.

The optional public extension `loomc/import/cxx.h` exposes
`loomc_module_import_cxx` to C embedders. It takes an ordinary immutable source
with UNKNOWN format, context, workspace, source configuration and allocator.
The returned module uses the existing compile, emit, link, query and serialize
APIs. A failed source produces a failed result with retained diagnostics;
allocation, provider and API failures return status without partial outputs.
Diagnostics own their source contents, including included headers.

The `//loom/binding/c/example/cxx:jit_amdgpu` example imports two HIP-style
kernels, specializes them to a live AMDGPU profile, emits one HSACO and executes
both through IREE HAL. Its host driver is C; its input kernels are C++.

Includes follow ordinary quoted, user-directory, system-directory and
`include_next` ordering. A source provider can replace filesystem reads with
immutable caller-owned headers, including a shared source cache. Each invocation
owns its mutable preprocessing and semantic state. Header hits and misses are
reused within an invocation. Diagnostic sinks copy any bytes they retain.

The importer embeds `loomcxx/` and `hip/` source headers by default. User and
system include paths take precedence over the embedded system root. The HIP
facade maps topology, synchronization, scalar half conversions and math onto
the Loom vocabulary; it does not load the HIP runtime or a host SDK.

`--builtin-includes=false --I=loom/src/loom/import/cxx/include` uses the external
headers. Building with `--//loom/config/import/cxx:embed_includes=false`, or
`-DLOOM_IMPORT_CXX_EMBED_INCLUDES=OFF` in CMake, also removes those header
contents from the library and its rebuild dependencies.

Canonical Python op declarations generate the typed `_Float16`, `float` and
`double` overloads in `loomcxx/scalar.h`, their documentation, and native
binding entries. For example, `loom::scalar::expf(x)` imports `scalar.expf`;
`loom::scalar::approximate::expf(x)` explicitly grants AFN. The HIP spelling
`__expf(x)` is an ordinary inline wrapper around that declaration. An explicit
`[[loom::op("scalar.expf", "afn")]] float custom_exp(float);` declaration uses
the same checked binding. Incorrect arity, types, flags, or attribute arguments
produce source diagnostics. Projection currently covers fixed homogeneous
floating-point operations; region, shaped-value and semantic-attribute
projections need their corresponding source representations.

Source standard, predefines, ABI triple, integer/pointer layout, and mathematical
approximation permissions are explicit options. `--std` selects the pinned
frontend's language and version macros; it does not promise historical-standard
conformance. LP64, LLP64 and ILP32 source layouts are independent of the machine
running the importer.

The current translation surface covers scalar arithmetic and conversions,
scalar-pointer indexing, local SSA values, conditional regions, counted and
general `for` loops, `while` and `do/while` loops, fixed workgroup arrays, and
direct calls. Unsupported reachable types and statements produce source
diagnostics. Pointer indexing
currently requires unsigned 32-bit source indices; extending it requires
preserving signedness and source pointer arithmetic in the address projection.
Objects with constructors, arbitrary pointer manipulation, general early
returns, exceptions and indirect calls need additional storage and control-flow
projections before they can be imported.
