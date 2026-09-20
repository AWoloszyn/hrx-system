# Source input providers

The input layer admits source text into an ordinary Loom module and retains the
source snapshots needed by later diagnostics. `loom-check`, `loom-link`, and
the execution tools use it before their existing compiler and runtime paths.
The frontend finishes and releases its AST before those paths run;
`loom_input_module_t` keeps the module, physical-to-logical source association,
and exact admitted source bytes through that boundary.

`loom_input_text_provider` is builtin. Optional importers own immutable
`loom_input_provider_t` descriptors beside their import implementation. Each
descriptor names its format and filename suffixes and invokes its native
frontend. The final binary supplies its linked provider list independently of
target or emit providers. Generic input and check libraries have no dependency
on an optional importer.

A provider receives borrowed source bytes, a physical input path, language
options, the parser diagnostic sink, and a source-capture callback. It captures
headers from every admission route before frontend storage expires. Source
rejection emits diagnostics and returns OK with no module; allocation, IO,
option, and sink failures propagate as statuses. Every load result is
deinitialized, including failures. Context and block pool outlive the result.

Physical paths own include lookup. Logical names are applied to diagnostics
and module source tables after capture, preserving source IDs and locations.
Distinct admitted sources cannot share a logical name. The retained resolver
uses module source IDs, so consumers cloning a module retain those assignments.
The linker publishes its input-to-output source mappings while materializing
the result. Execution sessions use those mappings to retain owned snapshots
indexed by the linked module's source IDs, including after input modules are
released. Later compilation consumes the retained mapping rather than matching
filenames or reopening source files.

Adding a native importer requires its descriptor and adapter, conditional
composition in each final binary, and importer-owned integration tests. The
shared test rules accept `.<format>-test`; envelopes, diagnostic matching,
output comparison, and updates stay common. Language options belong to the
provider. Multi-input tools accept `--input-options=format:options` once per
format and select each source or library by filename unless `--input-format`
overrides that selection. Bytecode remains a native input independently of
source providers. `loom_test` and `loom_library` pass source admission options
to the same linker. Projecting a source language into executable check cases
is an importer-owned language contract; the resulting operations use the
shared testbench and its existing invocation providers.
