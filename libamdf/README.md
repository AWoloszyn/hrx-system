# libamdf

libamdf is a small, portable C library for native AMD GPU and NPU access. It
provides the memory, capabilities, and execution resources for a unified
CPU/GPU/NPU fabric, with executable loading, command construction, scheduling,
and memory policy owned by the calling runtime, typically a hardware
abstraction layer (HAL).

The goal is one program across three execution domains. A CPU handles control,
a GPU prepares data and performs parallel computation, and an NPU executes
streaming and matrix workloads. Shared backing, explicit visibility, and
caller-owned lifetimes let those engines cooperate without turning each
boundary into a tensor copy or a separate runtime session.

libamdf targets RDNA and CDNA GPUs and XDNA NPUs on Linux, and RDNA GPUs and
XDNA NPUs on Windows. A common API exposes topology, locality, access, and
native queue capabilities so a HAL selects mechanisms by what they provide.

## What a common fabric enables

A compact model occupies one shared backing. GPU shaders prepare bounded
windows of weights in the layout needed by NPU workers, overlapping preparation
with computation. NPU results feed subsequent GPU or NPU work directly.
Expanded working storage follows the size of the pipeline window rather than
the size of another complete model copy.

On the NPU, tensor and channel state remain in SRAM as tile programs change
roles. Device-side transfers prefetch code while useful work continues.
Program lifetime follows the active computation; data lifetime follows the
values still needed by the pipeline. The compiler chooses between a larger
resident executable and a sequence of streamed programs.

Channels connect those stages at the granularity of useful data. Consumers
start from ready shards, independent readers retain their own uses, and storage
returns to a producer after its last reader finishes. Dynamic work enters
queues alongside data readiness. The CPU participates in application control
without acting as a payload relay or the scheduler of every internal stage.

[Loom](../loom/README.md) brings these pieces together in one language and
pipeline model. Arithmetic, communication, engine selection, and resource
reuse are part of the same program. libamdf supplies its native foundation
and remains independently usable by other compilers and runtimes.

[One fabric for CPU, GPU, and NPU programs](docs/fabric.md) develops these
scenarios and the architecture that makes them possible.

## A small boundary beneath the runtime

libamdf establishes backing and access through scope-taking memory operations.
The caller holds a memory handle and uses explicit ranges and addresses.
Discovery is passive, device creation is explicit, and allocation establishes
the requested consumer access before publishing the resource. The HAL owns
suballocation and retires memory before destroying its required native owners.

Execution uses caller-prepared native commands. A user queue exposes queue
state and doorbells; a kernel-mediated queue supplies the required platform
transport. GPU packet construction and XDNA executable loading, relocation,
and scheduling stay above the library. XDNA data memory is resident for its
allocation lifetime, without a per-invocation list of indirect data buffers.

This is the device-access foundation for a runtime that owns its execution
model. It replaces the need to adopt XRT or ROCr for native access while
keeping the compiler, loader, and scheduler with their caller. Family-selective
builds keep dependencies aligned with the hardware an application uses.
Static, shared-linked, and runtime-loaded clients share one C ABI.

The [memory design](docs/memory.md) describes storage, access, and ownership.
The [XDNA execution design](docs/xdna.md) describes instruction storage and the
native Linux and Windows submission boundaries.

## Native platform surface

libamdf is the portable C boundary for native AMD GPU and XDNA device access.
It isolates operating-system and driver-private mechanisms behind an
unloadable library while leaving executable formats, command construction,
scheduling, and memory policy in the calling runtime.

The base public surface is `include/amdf/amdf.h`. `amdf_query_api` negotiates an
ABI version and returns an immutable API table. Optional family surfaces in
`include/amdf/gpu.h` and `include/amdf/xdna.h` are negotiated from that table
according to what was compiled into the library, independent of the hardware
currently present. Querying any table performs no allocation, system call,
device discovery, dependent-library load, or other observable initialization.

An explicit provider instance owns the native platform state retained across
calls and can enumerate fixed-stride summaries of independently selectable AMD
execution endpoints. Opening an endpoint caches immutable identity and available
family-specific qualification without creating a device, address space,
paging queue, allocation, executable, or hardware queue, and without waking an
idle device. This permits host metadata storage, OS metadata handles and bounded
metadata queries; it does not imply zero system calls. If family qualification
fails, core endpoint identity remains queryable, but the failed family profile
is not published as a partial record. Builds without a native platform provider
expose the public headers but do not produce provider artifacts.

On Linux, endpoint qualification reads cached sysfs identity, topology and heap
metadata without opening a render, KFD or accelerator execution file. Expected
GPU memory limits come from explicit ISA and package descriptions; unknown
identities fail qualification instead of producing guessed memory capabilities.
Explicit device creation qualifies the installed native interface and obtains
the actual memory limits on the retained connection before VM initialization.
Live scope queries expose those limits without changing the endpoint snapshot.

On x86-64 Windows, the GPU extension qualifies an already opened KMT adapter
through adapter metadata queries and a private `amdf_wkmi_bridge.dll` runtime
companion. KMT adapter handles and the graphics kernel's process bookkeeping
are distinct from the driver process context and GPU address domain acquired
by explicit device creation. The bridge contains the pinned binary-only WKMI
C++ and CRT ABI behind a versioned C table. Successful qualification releases
the temporary WKMI adapter state and unloads the bridge before endpoint open
returns. GPU information queries copy the cached exact GFX identity, ASIC
revision, active compute geometry, LDS limit, and XCC topology without loading
a library or entering the driver. Expected memory profiles are qualified as a
complete set at the same boundary. GPU endpoints advertise kernel-published
PM4 and SDMA families only when the loaded KMT and WKMI surfaces provide
hardware-scheduled queues for the selected engine.

Each opened endpoint also reports a dense immutable set of native queue
families. A family identifies its accepted command representation (PM4, SDMA,
AQL, or XDNA) independently from how commands are published. User publication
means the caller directly updates mapped queue state; kernel publication means
a bounded provider call accepts already prepared top-level commands. CPU
translation from one representation to another is not a publication mode.

A materialized Windows GPU device owns one WDDM address domain and can attach
system, device-local, and registered-host memory at stable GPU virtual
addresses. Local allocations may be executable without being host-visible;
callers stage their contents through prepared GPU commands. The current
kernel-mediated PM4 and SDMA queues accept one immutable native command range
without reading or translating its bytes, borrow the command attachment until
a monitored progress fence retires it, and support bounded polling plus a
kernel wait.

Advertising a command/publication pair promises that a matching queue can be
constructed for that endpoint. Missing families mean the loaded provider has
no matching implementation, not that the silicon necessarily lacks the
capability. `endpoint_query_queue_family_info` copies records cached during
endpoint open and performs no allocation, system call, device initialization,
queue creation, retry, sleep, or device wait. A Windows endpoint advertises the
kernel-mediated XDNA family only when the loaded KMT surface contains every
operation required to construct and publish to that queue.

System memory scopes provide backing with one stable XDNA virtual address for
each requested live consumer on qualified Windows x86-64 systems. Memory creation
publishes the address only after mapping and ordinary residency have completed,
without using the fatal `MustSucceed` residency mode. Explicit host mappings expose
write-back cached pages and require range-scoped flush or invalidate operations
when ownership moves between the host and XDNA. Placement classes or properties
that the provider cannot fully satisfy fail explicitly instead of silently
degrading.

An XDNA context exposes its private instruction-memory scope through the same
memory API. The caller allocates executable backing, maps and publishes its own
bytes, and submits a memory handle, access ordinal, byte offset, and byte length.
ELF loading, PDI construction, relocation, argument layout and array scheduling
remain above libamdf. Instruction addresses and lengths satisfy the endpoint's
cached execution limits; submission does not read or modify instruction bytes.

The NPU4 and NPU5 profiles expose kernel-mediated XDNA queues with one instruction
range per submission and one unretired submission per queue. Submission is a
bounded native publication call: it performs no allocation, transaction
parsing, lowering, binding resolution, command transcription, retry, sleep, or
host wait. The queue owns mandatory platform packet storage. Status queries and
waits release the submitted memory borrow only after native fence completion
and command-result inspection. Indirectly referenced memory is resident for its
allocation lifetime and is not enumerated on each submission.

## Building and embedding

The build produces two link modes from one implementation:

- `//libamdf:amdf` and `amdf::amdf` consume the shared library.
- `//libamdf:amdf_static` and `amdf::amdf_static` consume the static library.
- `//libamdf:amdf_shared_artifact` names the loadable DLL or shared object for
  clients that resolve `amdf_query_api` at runtime.

Windows GPU distributions install the private WKMI bridge beside `amdf.dll`.
It is neither a public link input nor part of the libamdf ABI.

`AMDF_BUILD` controls the CMake subtree and defaults to `OFF`, independently
of HAL driver selection. The Bazel equivalent is
`--//libamdf/config:enabled`. RDNA, CDNA, and XDNA package admission is selected
with the `AMDF_FAMILY_*` CMake options or the
`--//libamdf/config:families=...` Bazel setting. Bazel implementation packages
select individual members through `//libamdf/config/family:rdna`, `:cdna`, and
`:xdna` without interpreting the setting themselves.

Configure and test Bazel explicitly without enabling the legacy AMDGPU HAL:

```bash
python dev.py bazel configure -DAMDF_BUILD=ON
iree-bazel-test --config=asan \
  --test_tag_filters=-iree-run-requirement=libamdf.resource.amd_gpu,-iree-run-requirement=libamdf.resource.xdna \
  //libamdf/...
iree-bazel-run //libamdf/examples:enumerate
```

The equivalent CMake build is:

```bash
iree-cmake-configure -DAMDF_BUILD=ON -DIREE_HAL_DRIVER_AMDGPU=OFF -DLIBHRX_BUILD=OFF -DLOOM_BUILD=OFF
iree-cmake-build amdf amdf_static
iree-cmake-test -R '^libamdf/' -LE 'manual|runtime-resource='
```

## Verification

Hardware-backed tests declare the `libamdf.resource.amd_gpu` or
`libamdf.resource.xdna` run requirement. CMake exposes the corresponding
`runtime-resource=amd-gpu` and `runtime-resource=amd-xdna` labels. These tests
remain discoverable by wildcard selection; host-only presubmit excludes their
requirements. Native suites share the AMD hardware resource group because GPU
CTS also exercises GPU/XDNA interoperability.

Each CTS corpus runs with both native lifetime policies across static, shared,
and dynamically loaded providers. The `static`, `shared`, and `dynamic` targets
use the default process lifetime; their `_instance` counterparts require bounded
instance lifetime. The same test cases query capabilities: host-registration
scenarios run wherever registration is supported, and native-owner recreation
scenarios require reclaimable VM acquisition. Ordinary memory, queue, and interop
cases share one device per endpoint for the duration of each test process.

On a qualified XDNA host, select all XDNA hardware tests, including each CTS
linkage mode and the native DRM device lifecycle:

```bash
iree-bazel-test --config=asan --//libamdf/config:families=xdna \
  --test_tag_filters=iree-run-requirement=libamdf.resource.xdna //libamdf/...
iree-cmake-test -L runtime-resource=amd-xdna
```

Linux XDNA execution requires the host's `amdxdna` driver and `/dev/accel`
devices to be accessible inside the test environment. GPU device access through
`/dev/kfd` and `/dev/dri` does not provide XDNA device access.

Installing the repository exports `amdf::amdf` and `amdf::amdf_static` through
`find_package(amdf CONFIG REQUIRED)` and installs the public headers beneath
`include/amdf/`.
