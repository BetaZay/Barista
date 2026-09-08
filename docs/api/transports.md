# Transports and VM Direction

The shared C++ contracts are the API. D-Bus, AppHook, and a future VM channel
are adapters with different trust and serialization needs.

## Current transport matrix

| Transport | Control | Video/audio | Input | Scope | State |
| --- | --- | --- | --- | --- | --- |
| In-process C++ contracts | Yes | Yes | Yes | Portable interface | Defined |
| System D-Bus | Yes | No | No | Linux host | Implemented |
| AppHook/MUG1 | No | Yes | Yes | Same Linux user/session | Implemented |
| Legacy `/tmp/drcd.sock` | Legacy commands | No | No | Linux host | Compatibility only |
| VM/network transport | Planned | Planned | Planned | Host to bundled guest | Not implemented |

No stable TCP port, VM discovery mechanism, or network wire schema exists
today. Code must not treat MUG1 packets or D-Bus variant maps as that schema.

## Recommended bundled-VM shape

Keep the same application API on every platform and put the platform-specific
radio stack behind one backend adapter:

```text
app/gui + application connector
             |
       barista::api facade
             |
     host transport adapter
             |
   authenticated VM channel
             |
 guest SessionService + media adapter
             |
        DRH radio engine
```

The host adapter should implement or proxy `SessionService`, `MediaProducer`,
and `InputConsumer`. The guest should translate those requests into the same DRH
server/encoder calls used by the native Linux backend. This preserves one API
for the GUI and third-party applications and keeps hypervisor choice out of
`core/api`.

## Requirements for a VM wire protocol

A first protocol version should define:

1. A framed, versioned handshake containing API version, capabilities, role,
   and a fresh session nonce.
2. Structured request IDs, responses, and the shared error categories for every
   `SessionService` operation.
3. Explicit serialization for every [`SessionStatus`](status.md) field, with
   unknown-field tolerance.
4. Separate bounded channels or queues for control, latest-frame video, audio,
   and input so control cannot be starved by media.
5. Authentication tied to the VM instance and host user. TCP loopback alone is
   not an authentication boundary.
6. Deadlines, cancellation, heartbeat behavior, maximum message sizes, and
   reconnection semantics.
7. A negotiated media format. Do not assume host-native integer layout or send
   C++ structs with padding across the boundary.

For a local bundled VM, a hypervisor socket such as virtio-vsock is preferable
when available because it avoids choosing and exposing a host TCP port. A
loopback TCP fallback can use an ephemeral port passed through a protected
launcher channel. Either choice should sit behind the same transport interface.

Container runtimes can package user-space dependencies, but they do not by
themselves solve direct access to the required Wi-Fi adapter/driver. On macOS,
Apple's container facilities still use a Linux virtual-machine boundary, so the
backend needs an explicit, tested device-access strategy as well as this API
transport.

## Migration sequence

1. Make the Linux service implement the shared `SessionService` behavior
   directly, leaving D-Bus as a thin codec/authorization adapter.
2. Add transport-neutral conformance tests for validation, phases, ownership,
   errors, and capability reporting.
3. Define a versioned VM control schema from the shared types.
4. Add bounded media/input channels with the same latest-frame policy as
   `MediaProducer`.
5. Run the same conformance suite against native Linux and VM-backed adapters.
6. Keep AppHook as the application connector unless a cross-platform connector
   transport is deliberately introduced and versioned.

This sequence allows the VM work to proceed without moving privileged radio
logic into the GUI or maintaining two behavioral APIs.
