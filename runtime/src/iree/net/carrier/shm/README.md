# Shared-Memory Transport

The SHM carrier connects independent processes on one host through the generic
`iree/net` connection, message endpoint, and session interfaces. It is useful
for local services, collectives, and remote HAL, but has no dependency on HAL
or any device driver. Applications that need only mappings or native resource
exchange can use the lower layers independently.

The transport supports Linux, Windows, and macOS 14.4 or newer. Apple builds
require an SDK providing the public shared-address wait APIs. The runtime can
still target older macOS versions: factory construction reports `UNAVAILABLE`
there, without preventing other transports from loading or operating. A Unix
socket or local named pipe establishes the connection and transfers an anonymous shared mapping and
wake resources. Messages then move through shared payload slots; the native
stream remains open to detect peer departure. Progress runs on the caller's
proactor, without a transport worker, background registry, or polling timer.

## Using The Factory

Link `//runtime/src/iree/net/carrier/shm:factory` in Bazel or
`iree::net::carrier::shm::factory` in CMake. Both expose
`runtime/src/iree/net/carrier/shm/factory.h`:

```c
iree_net_shm_factory_options_t options =
    iree_net_shm_factory_options_default();
iree_net_transport_factory_t* factory = NULL;
IREE_RETURN_IF_ERROR(iree_net_shm_factory_create(
    &options, host_allocator, &factory));
```

Pass this factory to `iree_net_transport_factory_create_listener` and
`iree_net_transport_factory_connect`, or use `iree_net_session_connect` and
`iree_net_session_accept` for session bootstrap and control traffic. The generic
`receive_pool` argument is unused by SHM. Factory construction is explicit;
creating a factory does not bind an address or start a thread. An application
using the optional transport registry can register the factory under `shm`.

On Windows, the supplied proactor must enable `WAIT_COMPLETION_PACKET` for
persistent control-pipe monitoring. Listener creation and outbound connect
admission return `UNAVAILABLE` before acquiring resources when that capability
is absent. One-shot legacy wait support alone is insufficient.

Native address syntax is:

| Platform | Address | Ownership And Access |
| --- | --- | --- |
| Linux/Android | `@my-service` | Abstract Unix socket name, scoped to the network namespace. No filesystem permission policy. |
| POSIX | `/run/user/1000/my-service.sock` | Filesystem Unix socket. The containing directory and socket permissions control access. |
| Windows | `my-service` | Local named pipe name without the `\\.\pipe\` prefix or path separators. The process's default security descriptor applies; remote pipe clients are rejected. |

Filesystem paths must fit the platform's Unix socket address limit. An existing
path is never removed to make a bind succeed. Listener stop checks its pathname's
device/inode identity before unlinking. Applications manage discovery,
authorization, stale paths, and changes to their namespace. A busy Windows pipe
reports `UNAVAILABLE`; connection setup does not block in `WaitNamedPipe` or
silently create retry work.

Both proactors must progress during establishment. Public success means resource
import has completed and the connection can open endpoints, not merely that a
socket connected. Endpoints are paired by open ordinal, so both peers open them
in the same protocol-defined order. The endpoint views borrow their connection.
Sessions reserve their control endpoint before application endpoints.

`iree_net_transport_connect_operation_t` gives a pending connect a stable,
caller-owned cancellation identity. Keep it alive through initiation return,
the terminal callback, and any concurrent cancellation calls. Cancellation is
asynchronous and does not revoke a connection already published to the caller.
The factory can be released after the initiating call; admitted setup owns what
it needs independently.

## Data And Ownership

Every endpoint has two independent directions. Each direction contains a fixed
set of payload slots, a shared free-index list, a descriptor queue, and a
consumption receipt. Descriptors identify slots and stream positions, never
process-local pointers. Payload storage and descriptor lifetime are separate:
consuming a descriptor does not imply that the application released its bytes.

On send, the carrier reserves one bounded operation record and copies source
span descriptors. It generates the transient prefix before returning, outside
the admission lock. Private source bytes remain borrowed until the completion
callback. The poll owner copies prefix and source bytes into as many shared
slots as necessary, publishing descriptors in admission order. A slow prefix
writer does not hold a lock or block the polling thread.

Source completion follows the peer's consumption of the final descriptor. It
does not wait for the receiving application to release the message. This is
safe because the peer sees shared-slot copies, not the original source. The
carrier advertises reliable, ordered delivery, not zero-copy transmit or access
to arbitrary GPU addresses.

Complete messages in a native slot can transfer that slot's receive lease to
the application without another payload copy. The application moves a lease by
taking it and clearing the callback's lease, and later returns it with
`iree_async_buffer_lease_release`. Only `N-1` of `N` slots per direction can be
retained this way. The last slot remains available for progress: its borrowed
bytes go through the framing adapter, which supplies independently owned message
storage. Holding every delivered message therefore cannot prevent a subsequent
completion, credit, or control message from arriving.

Messages that span slots use the same framing adapter to assemble contiguous
owned storage. Slot capacity is not a message-size limit. The framed endpoint
accepts nonempty messages up to `UINT32_MAX - 8` bytes, including generated
prefix and payload. Larger logical transfers use multiple messages, such as
the bulk channel's credit-bounded DATA records and 64-bit transfer offsets.
Large model weights are not required to fit in a slot or in one message.

Each native receive lease retains only detached mapping and wake ownership.
It can outlive the connection, its proactor, and the process that created the
mapping. Different consumer threads may release their uniquely owned leases
concurrently. A lease does not retain the proactor or require the sender to
stay alive. This is an ownership guarantee for cooperating participants, not
protection against a peer deliberately modifying shared pages.

## Capacity And Progress

The defaults provide four endpoint ordinals, sixteen 64 KiB slots per direction,
sixteen admitted sends per endpoint, and 16 KiB of inline generated-prefix
storage per send record. Larger prefixes receive exact-size private storage;
they are not rejected at the inline-capacity boundary.

Four bidirectional endpoints reserve 8 MiB of shared payload per connection,
plus metadata. Sixteen connections reserve about 128 MiB of shared payload.
Payload pages are not touched during construction. Each process also preallocates
the send records and inline prefixes, approximately 1 MiB of prefix storage per
connection at the defaults. Endpoint/slot counts and capacities are configurable
in the factory options. Receive retention and fragmented messages may allocate
additional application-owned storage.

The listener offers its exact geometry. The client treats its own configured
dimensions as resource limits and rejects larger offers before mapping or
endpoint allocation. `max_pending_connections` bounds simultaneous acceptance
and import handshakes, not established connections. Completed handshakes return
their setup slot before invoking the application's accept callback.

Send admission is independent of payload availability: an accepted operation
owns its progress even when every immediately reusable slot is busy. A zero
send budget means admitted operations must complete before more can enter.
Completion releases the record before invoking the caller, permitting the next
send directly from that callback. No sleep or periodic retry is needed.

Each connection has one native notification bundle per receiving side. Its
eight-byte shared state couples the epoch with blocking-wait enrollment.
Linux uses an eventfd for async readiness and a shared futex for blocking waits;
macOS uses a pipe and public shared-address waits. Windows uses separate
auto-reset events for async and blocking observers, with local caller handoff
instead of a notification worker. Publication skips the extra synchronous wake
when no blocking callers are enrolled.
Its endpoint waits subscribe to the same local notification. Publication and
immediate slot returns share a batch wake; detached lease returns signal their
peer directly. Epoch checks make coalescing safe: a native wake is advisory,
while the shared state determines whether progress exists.

## Failure And Teardown

Bootstrap is an exact-version, little-endian `OFFER`, `ACCEPT`, `READY` exchange.
The server holds exported resources through `ACCEPT`, which acknowledges receipt
of tentative client imports. `READY` confirms that the server held those resources
through that acknowledgment. Until then, client imports remain owned but unused:
mapping, native wake registration, and connection construction follow `READY`.
Client construction failure closes the stream so the server observes peer loss.
Setup cancellation joins native operations and resource ownership before its
public completion callback.

After publication, native stream EOF terminates the connection and its admitted
sends. Shared mappings alone cannot indicate peer exit. Connection deactivation
closes admission and joins endpoint progress, pending endpoint-ready callbacks,
native observation, and cold error handoffs. It does not wait for application
leases. Free the connection only after its deactivation callback; stop and free
a listener using the analogous stopped callback. Listener shutdown does not
revoke connections that were already published.

Doorbell publication is infallible and does not acknowledge peer progress.
Locally retained native resources remain valid after peer departure, so detached
lease returns require neither a live connection nor a connection lock. Peer and
asynchronous transport failures record a terminal error and hand it to the poll
owner. Detachment joins that weak error handoff before proactor destruction.
Applications own progress deadlines for connected but stalled peers; a timeout
does not replace the deactivation callback's ownership join.
Peer-specific bootstrap failure retires only that handshake; a native listener
setup failure stops further acceptance and reports an error. Explicit listener
stop still provides the final ownership join.

## Reuse Beyond Remote HAL

The useful boundaries are distinct, and a consumer can start at the one it needs:

| Requirement | Existing Interface |
| --- | --- |
| Create, map, duplicate, or close host shared memory | `runtime/src/iree/base/internal/shm.h` |
| Transfer native resources over a local byte stream | `runtime/src/iree/async/util/local_stream.h` |
| Own native events independently of an executor | `runtime/src/iree/async/event.h` |
| Publish and synchronously await shared epochs without an executor | `runtime/src/iree/async/notification_native.h` |
| Wait for shared epochs on an application's proactor | `runtime/src/iree/async/notification.h` |
| Connect local processes and exchange leased messages | `runtime/src/iree/net/carrier/shm/factory.h` |
| Carry control messages, bulk transfers, or queue protocols | `runtime/src/iree/net/session.h` and `runtime/src/iree/net/channel/` |

For HIP-on-libhrx-on-HAL, this supplies host mappings, native resource transfer,
wake ownership, and optional generic messaging without linking remote HAL or an
unrelated HAL driver. A native handle is process-local; copying its integer
value into an IPC token is not resource transfer. `local_stream` establishes
independent native ownership, while the consuming protocol defines when import
has completed and when application resources may be reused.

A host shared mapping is not automatically a device allocation, a GPU-importable
allocation, or an IPC event. Device visibility, GPU memory import, queue ordering,
and any public HIP token semantics belong to their HAL/application contracts.
The transport's private slot layout is not an allocation registry or a general
GPU-memory format. Consumers needing a shared host slab can exchange its handle
through the lower layers without adopting the message carrier's slot layout.

## Development

The implementation is split by ownership: `region` calculates shared geometry,
`storage` owns detached mappings and wakes, `carrier` moves ordered bytes,
`connection` joins endpoints and peer lifetime, and `handshake`, `connect`, and
`listener` own native setup. Their sources are under
`runtime/src/iree/net/carrier/shm/`. Shared framing lives in
`runtime/src/iree/net/framed_endpoint.h`, with no second SHM message parser.

Run the focused tests and the transport-independent CTS with:

```sh
iree-bazel-test --config=asan //runtime/src/iree/net/carrier/shm/...
iree-bazel-test --config=tsan //runtime/src/iree/net/carrier/shm/...
```

`factory_process_test` starts independent processes, retains received data,
terminates the creator without orderly transport teardown, and releases the
leases after destroying the receiving connection and proactor. `factory_test`
holds real native peers at import-acknowledgement boundaries while cancelling
or stopping the other side. Carrier tests exercise saturation, prefix-writer
concurrency, retained-slot pressure, endpoint isolation, and completion reentry.
The bootstrap codec has its own tests and fuzzer.
