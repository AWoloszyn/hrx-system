# VM target

The VM target compiles ordinary Loom functions into standalone `.vm` modules.
It uses the same legalization, control-flow lowering, scheduling, and register
allocation infrastructure as native targets. The VM package supplies the
machine description and bytecode emission, not a separate compiler pipeline.

## Compile a function

```mlir
func.def public @arithmetic(%a: i32, %b: i32, %c: i32) -> (i32) {
  %sum = scalar.addi %a, %b : i32
  %scaled = scalar.muli %sum, %c : i32
  func.return %scaled : i32
}
```

With a VM-enabled compiler, save this as `arithmetic.loom` and run:

```sh
loom-compile arithmetic.loom --target=vm:core --output=arithmetic.vm
vm-dis arithmetic.vm
```

`--target=vm:core` selects the VM profile and its default `.vm` output format.
The source needs no target declaration or attributes. The shared compiler
specializes public functions and their callees for the selected profile, which
supplies the calling convention during lowering. A public function exports its
symbol name unless an explicit export name overrides it. Internal callees are
compiled into the same module without becoming host-callable exports.

The output contains the instruction stream, logical VM signatures, sorted
callable and export tables, and referenced read-only data. It is independent of
the source module and compiler lifetime. A host loads it through the bytecode
module API, links a program, creates a process, and invokes an export. The
[runtime guide](../../../../../../runtime/src/iree/vm/README.md) explains those
objects, argument/result ownership, and reusable invocation storage.

## From source to bytes

Source operations first use the shared target specialization and legalization
pipeline. Supported scalar operations select VM descriptors; vector bodies can
use shared scalarization where their operations and control-flow boundaries
permit it. Source functions then become `low.func.def target<vm.core>` with
typed value and reference registers. Low assembly uses the VM mnemonics, while
retaining compiler constructs such as SSA values and block arguments.

The common frame builder schedules those Low operations, assigns registers and
local storage, and plans edge copies and spills. [function.c](function.c)
consumes that frame to write instruction packets, direct calls, transfers, and
branches. Branch displacements are patched after their target positions are
known. The emitter does not rerun whole-module verification or reconstruct a
second allocation plan.

[module.c](module.c) collects VM functions from the prepared module and assigns
their table ordinals. It consumes shared function-version information instead
of performing its own callgraph specialization. Only referenced read-only
payloads are included. Fixed table rows are reserved and patched around a single
instruction-emission pass into a segmented stream. The returned byte sequence
owns its storage; consumers can enumerate it or explicitly clone it when they
need contiguous bytes.

## Where the contracts live

| Area | Owner |
| --- | --- |
| Opcodes, packet fields, selector semantics, and wire records | [Runtime ISA specification](../../../../../../runtime/src/iree/vm/bytecode/spec/) |
| Projection into shared Low descriptors | [descriptors.py](../../../../../py/loom/target/arch/vm/descriptors.py) |
| Source-operation correspondence and selection constraints | [contracts.py](../../../../../py/loom/target/arch/vm/contracts.py) |
| Target registration and profile selection | [provider.c](provider.c) |
| Type mapping and symbolic read-only data lowering | [lower.c](lower.c) |
| Selection of native math forms or shared recipes | [math_policy.c](math_policy.c) |
| Frame and module emission | [function.h](function.h), [module.h](module.h) |

The Python projection imports the runtime specification. It maps the existing
ISA into Loom's descriptor and constraint schemas instead of maintaining a
parallel set of opcode numbers or packet layouts. Generated contract indices
and descriptor tables are build outputs, consumed as immutable data by the
shared lowering machinery and the target emitter.

## Supported source boundary

This compiler surface supports synchronous ordinary functions with scalar and
buffer arguments/results, direct internal calls, branches and loops, local
storage, typed memory operations, and referenced read-only data. Scalars
include narrow integer and floating formats; `index` and `offset` use the
profile's 64-bit carrier. Internal vector programs rely on shared legalization,
not a public vector calling convention. Supported math modes are selected by
the target math policy rather than by substituting a host platform's libm.

Runtime ISA availability is distinct from source-lowering support. This target
does not provide kernel/workgroup execution, HAL command programs, runtime
function imports, indirect calls, process-global mutation, or suspension.
Unsupported source representations and instructions fail compilation; the
emitter does not fall back to another execution engine. Kernel launch-config
evaluation is a separate integration boundary, not part of compiling an
ordinary `.vm` module.

## Testing

[test/](test/) contains Low assembly, emission, and source-lowering checks.
Portable lowering inputs come from the shared `source_low` TEMPLATE corpus;
VM expectations specify the target's selected representation.

The [function correctness corpus](../../../test/corpus/functions/) contains
target-neutral functions and their `check.case` inputs and expectations. Its VM
execution profile runs through `iree-test-loom`, using the
[VM testbench](../../../tooling/target/vm/testbench.h). The testbench compiles
one module for all its cases, releases the compiler copy and scratch before
loading the emitted bytes, and reuses a runtime process and invocation storage.
This exercises artifact ownership, dynamic execution, and returned buffer
aliases rather than only comparing disassembly.

The [tool integration suite](../../../tooling/target/vm/vm.test.json) compiles
source with `loom-compile` and reads the resulting file with `vm-dis` in a
separate process. Runtime instruction and malformed-image tests remain under
`runtime/src/iree/vm/`; they do not require the compiler.
