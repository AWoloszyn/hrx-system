# Source input providers

The input layer admits source text into an ordinary Loom module and retains the
source snapshots needed by later diagnostics. `loom-check` uses it before its
existing compiler modes. The frontend finishes and releases its AST before
those modes run; `loom_input_module_t` keeps the module, physical-to-logical
source association, and exact admitted source bytes through that boundary.

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
uses module source IDs, so consumers cloning a module retain those assignments
or construct a translated source table at their own linking boundary.

Adding a native importer requires its descriptor and adapter, conditional
composition in each final binary, and importer-owned integration tests. The
shared test rules accept `.<format>-test`; envelopes, diagnostic matching,
output comparison, and updates stay common. Language options belong to the
provider. Execution tools can consume the same loaded module and source
lifetime; projecting source programs into executable check cases is a separate
language contract.
