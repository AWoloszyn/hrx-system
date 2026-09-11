# Memory fabric

libamdf obtains storage, establishes access to it, and describes the rules for
using it. Allocation policy, suballocation, scheduling, executable loading, and
deciding when storage can be reused belong to the caller, typically a hardware
abstraction layer (HAL).

This document defines the memory design and its caller scenarios. Capability
discovery identifies the contracts a particular implementation provides; this
design is not a blanket claim of backend support. Public headers specify the
available C ABI.

## Fabric discovery

A libamdf instance is the application's explicit lifetime root. Through it, the
application discovers participating engines and memory resources: CPUs, GPUs,
NPUs, system-memory locations, GPU-local memory, and their supported access
relationships.

Discovery is passive. It supplies enough information to select hardware and plan
memory without creating execution devices, queues, firmware contexts, address
spaces, or device allocations. Placement, alignment and address-width limits,
supported access mechanisms, and expected sharing capabilities are complete
facts for discovery, not partially populated live-device structures. Explicit
resource creation produces complete information about what was actually
obtained. Caller policy handles refinements; explicit requirements remain
binding.

The fabric does not imply that every engine can access every allocation. It
presents resources and relationships together so callers can reason about them
without assembling separate platform-specific memory systems.

## Scopes and profiles

A memory scope answers: what storage can be obtained here, under what access and
lifetime rules? It is not itself an allocation, pool, device, or address space.

| Scope | Storage and access | Discovery |
| --- | --- | --- |
| Fabric system memory | System backing usable by a supported set of CPU, GPU, and NPU consumers. | Available before device initialization. |
| Physically local memory | Storage in an identified location, such as one GPU's VRAM or HBM. Peer and host access depend on capabilities. | Available passively where the contract is knowable without activation. |
| Native-private memory | Storage whose meaning or accessibility depends on a particular device or context. | Retrieved from that live owner. |

Discovering a scope makes its storage contract available for selection. It does
not initialize the devices needed to obtain that storage or establish access.
The caller explicitly initializes those devices before device-dependent memory
construction. CPU-only system-memory allocation requires no accelerator.

Physical locality identifies where backing lives; it does not make the public
allocation interface an operation on one originating device. Shared access is
established through the scope-taking operation, including for local memory when
supported. A context-private aperture has the narrower access and lifetime rules
of its actual owner. The same scope-taking memory operations serve all three
cases.

Within a scope, memory profiles describe supported combinations of placement,
construction method, access, alignment, host mapping, and sharing. A profile is
a valid combination, not a collection of independently composable flags that
can accidentally describe an impossible request.

## Backing and resource handles

An `amdf_memory_t` represents one backing store and the access established for
it. Several engines can reach that backing without a separate public memory
object for each engine.

There are three ways to obtain a memory resource:

- Allocation obtains new backing owned by libamdf.
- Registration accepts caller-owned storage. Libamdf owns the registrations,
  pins, and mappings; the caller owns the original storage and keeps it alive.
- Import acquires the documented ownership or reference to externally supplied
  backing and establishes the requested access.

Registration and import are acquisition mechanisms, not physical locations.
Imported memory can be system memory or GPU-local memory.

A construction request specifies the scope and profile, size, alignment,
placement requirements, and consumers and access it needs. Device consumers
identify devices the application has already initialized. Scope capabilities
describe supported access combinations; the request selects the access that
must be established for this allocation. For example:

> Allocate system memory that the CPU can map, this GPU can read and write,
> and this NPU's DMA engine can read.

Success establishes the entire requested contract. A second copy, weaker
access, or an unprepared consumer is not a successful substitute. Native
handles, registrations, peer mappings, and backing references are construction
details owned by the resource. External import and export serve interoperability;
ordinary sharing within the fabric does not require the application to assemble
an export/import chain.

Resource operations identify memory with its opaque handle and an explicit
range: conceptually `{memory, offset, length}`. Host-mapping operations similarly
use their mapping handle. Libamdf does not reverse-map arbitrary physical or
virtual addresses to allocations through a global registry, hash table, or
interval search. The caller already has the handle identifying the resource.
An opaque handle does not require an integer handle table; its representation
can directly identify the library-owned object.

Host registration receives a pointer and length at the acquisition boundary;
subsequent operations use the resulting handle. Address queries return pointers
or numeric addresses for CPU and device use, not identifiers to feed back into
an allocation lookup. Native imports likewise carry an explicit transport and
ownership contract rather than asking libamdf to infer an owner from an address.

A HAL can allocate a large slab, retain its handle, and suballocate using offsets.
An interior range is explicit and needs no search for an original allocation
base. Libamdf needs no object for every tensor, argument block, or executable
subrange.

## Addresses and execution domains

An address is meaningful to a particular consumer in a particular address domain:
the environment in which the numeric address is interpreted. One backing store
can have a CPU mapping, a GPU virtual address, an XDNA DMA address, and a different
address interpretation required by firmware.

Different numeric addresses can identify the same bytes. Equal numeric addresses
do not prove shared backing. An address result describes its domain, intended
consumer, accessible range, and granted access. The caller can cache the base
and use offsets within that range. Querying it is a metadata operation, not a
hidden mapping or residency operation.

Scopes and address domains are distinct. Several placement scopes can use one
domain; one scope can support access through several domains. Queues use the
mappings established for their initialized devices. Creating a queue does not
require another public memory attachment or a new copy of the backing. Native
isolation requirements remain explicit; sharing a libamdf instance does not by
itself guarantee native address space isolation or sharing.

## Native resources and activation

The instance owns shared native driver connections. Explicit device creation
initializes the participating device and establishes the device-specific native
state needed by its supported operations. Instance creation and passive scope
discovery do not initialize every available accelerator.

The instance has no allocation registry or deferred memory-release list. Memory
bookkeeping belongs to the individual resource, including when construction
fails; sharing a driver connection does not make the instance a memory owner.

An `amdf_memory_t` owns its backing references, allocation handles, registrations,
and established access. A platform can require native device handles, virtual
memory state, or paging queues for those operations. Those dependencies are
concrete implementation state, not a second public allocation owner or a reason
to create a hidden public device during memory construction.

For example, Windows allocation APIs use native device handles, and GPU address
mapping requires synchronization with a paging fence. Those obligations belong
beneath the portable contract; they do not require the application's first
execution queue to own shared memory. See the [Windows allocation API] and
[GPU virtual-address management].

Devices required to obtain or access the allocation are explicit lifetime
dependencies. The caller keeps them alive until the dependent memory is
released. Instance ownership of shared connections does not retain a destroyed
public device or extend the validity of its dependent memory.

Memory construction performs the native allocation and access setup required
by its scope and requested consumers. It does not implicitly initialize a
participating device. CPU-only system memory does not activate a GPU or NPU;
ordinary data allocation does not create workload contexts or load a program.

## Readiness, visibility, and ordering

Readiness means the requested backing, mappings, pinning, and residency
requirements have been established. Ordinary memory construction returns ready
resources. Native setup costs occur at that resource boundary, not once per
dispatch. There is no per-invocation indirect BO list, first-launch registration,
hidden pinning submission, or scan of the application's pointer graph.

Visibility describes how one participant observes another's writes. Shared
backing does not automatically imply coherent caches or mutually supported
atomics. Libamdf exposes directional rules for the actual producer, consumer,
memory, and access mechanism. These identify required CPU or engine-side cache
operations, or establish that no cache maintenance is needed. Atomic support
includes the relevant operation, width, and participating domains, not merely
a property saying that memory supports atomics.

Ordering establishes when the consumer may act. Cache coherence does not create
a producer-to-consumer dependency. The HAL or application supplies that
dependency through its synchronization and scheduling mechanisms. Libamdf
exposes native mechanisms and requirements without inferring dependencies from
pointers or generating a scheduling policy. The HAL constructs engine commands;
the library does not need a packet builder to describe a cache requirement.

## Executable storage

Storage intended for execution is `amdf_memory_t` with the appropriate
executable-use contract. Its scope and profile describe which consumer can use
it and any native restrictions. A buffer containing machine code is not
automatically executable memory.

A catalog of tile images in DRAM, read by DMA and streamed into tile instruction
memory, is ordinary readable backing from the DMA consumer's perspective.
Storage passed directly to a firmware execution interface can require a
different scope and address interpretation. Libamdf exposes storage and access
supported by the native interface, not an assumed allocator for every tile-local
resource.

Libamdf supplies storage, addresses, access guarantees, and native resource
mechanisms. The caller supplies the bytes and understands their meaning. ELF
loading, relocations, PDI construction, tile-control tables, replacement
programs, and streaming schedules belong above the library. A native path's
requirement for setup material is explicit; it does not give libamdf ownership
of the application's program layout or schedule.

## Lifetime and failure

Device-dependent memory is constructed after the required devices have been
initialized and is released before those devices are destroyed. Private memory
also depends on the device or context identified by its scope. These are caller
preconditions: libamdf neither retains those owners through memory nor tracks
their memory descendants to diagnose premature destruction. Destroying a
required owner while dependent memory remains live is application misuse.

Ordinary memory can precede workload queues and remain usable after a queue is
destroyed, while its required devices remain live. A context-private allocation
has its scope's narrower lifetime. The lifetime of a queue, a context, and a
device are distinct contracts, not interchangeable meanings of an execution
object.

An external import acquires the reference or ownership defined by its transport.
Its backing can remain valid after an independent source reference is released.
That is not continued validity of the source memory handle or permission to
destroy a device on which the surviving import itself depends.

The caller establishes last use before destroying or reusing backing. Under
user-mode scheduling, completion of a kernel submission does not necessarily
mean that no tile, queue, DMA engine, or subsequent program can follow a pointer
into the allocation. Libamdf cannot discover references embedded in opaque
commands or device-side queues. Destruction performs no implicit wait for all
possible uses, and a successful destruction is not proof of device retirement.

Caller lifetime obligations do not remove public range and capability
validation or native error reporting. Constructors own rollback of the resources
they acquire before publication. They publish complete outputs only on success;
failure leaves caller output storage unchanged and creates no caller cleanup
obligation. A cleanup error does not transfer memory ownership to the instance.

Native error handling follows the particular operation's resource-consumption
and progress contract. An interrupted operation with documented resumable
progress differs from a terminal release failure. An error alone does not
establish that retaining an object and retrying later is safe or useful, and
reporting failure does not imply that a native resource has been released.

If construction rollback encounters a terminal native release failure, it
reports that cleanup error and frees its unpublished bookkeeping. Unreleased
native resources and any library-owned backing or address reservation they
still depend on are leaked, not transferred to a retry list. A driver failure
cannot be repaired by retaining more libamdf objects. Native residue is left to
native teardown; leaked host reservations can remain until process exit. The
native lifetime policy describes supported reclamation boundaries, not a
guarantee of reclamation when the native release operation itself fails.

Normal execution teardown differs from physical loss. Destroying a GPU queue
need not invalidate shared system memory. Losing the GPU holding an HBM
allocation can lose that backing; there is no invisible surviving copy. A failed
projection does not automatically imply that the shared backing or another
participant's projection was lost.

## Caller scenarios

A CPU/GPU/NPU pipeline follows one resource lifecycle:

1. Create an instance and passively discover endpoints, available scopes,
   profiles, and access capabilities.
2. Select a system-memory contract covering the consumers and their address
   domains.
3. Explicitly initialize the GPU and NPU devices. Check their complete achieved
   capabilities against the selected requirements.
4. Allocate backing once from the selected scope, requesting access for those
   live devices. Obtain a CPU mapping and cache the established device addresses.
5. Create workload queues and any required contexts. Retrieve private scopes from
   their live owners and allocate private executable storage where needed.
6. Run the pipeline with explicit visibility operations and dependencies, reusing
   backing and cached addresses across submissions. Retiring and destroying a
   workload queue does not require rebuilding ordinary memory access.
7. Establish final device-side last use and release resources in dependency
   order: commands and queues before memory they borrow, host mappings before
   their memory, and private memory before its owning context or device.
8. Destroy the now-unused devices, close endpoints, and destroy the instance.

A discrete-GPU pipeline can instead select local VRAM or HBM and require access
from particular peer GPUs. If a required peer or NPU cannot access that placement,
construction fails. The HAL can explicitly choose separate system memory and a
scheduled transfer; libamdf does not manufacture shared backing with copies.

An application registering existing host storage keeps that storage alive
through registration and every device use. An application importing external
memory follows the transport's ownership contract. Both then operate with
handles and ranges through the same scope-based interface.

Virtual-memory operations fit alongside this model. Reserving address space is
separate from allocating backing. Mapping, aliasing, or remapping names explicit
resource handles and ranges and has its own ordering, completion, and lifetime
contract; it is not ordinary submission preparation. Unsupported contracts are
reported through capability discovery and rejected when requested.

Budget reporting is a separate, explicitly supported query. Logical backing is
counted once; native mapping overhead and platform budget charges are separate
facts. Budget queries do not introduce allocation policy or fabricate unavailable
fields.

Libamdf owns the native machinery that makes memory usable. The caller owns what
the bytes mean, how work moves through them, and when they can be reused.

[Windows allocation API]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtcreateallocation
[GPU virtual-address management]: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/per-process-gpu-virtual-address-spaces
