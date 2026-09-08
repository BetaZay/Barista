# Control API

The platform-neutral control contract is
[`barista::api::SessionService`](../../core/api/service.h). It owns session
lifecycle and saved-GamePad operations; it does not expose Wi-Fi credentials,
radio packets, process management, or encoder internals.

## Operations

| Method | Request | Result |
| --- | --- | --- |
| `GetStatus()` | None | Current [`SessionStatus`](status.md) |
| `ListGamePads()` | None | Saved `GamePad { mac, name }` records |
| `PrepareSystem()` | None | Ensures required host facilities are available |
| `StartSession(request)` | `StartSessionRequest` | Starts an already-paired GamePad session |
| `Pair(request)` | `PairRequest` | Pairs a GamePad, then starts a session |
| `StopSession()` | None | Stops the caller-owned session |
| `RenameGamePad(request)` | `RenameGamePadRequest` | Updates a saved display name |
| `RemoveGamePad(request)` | `RemoveGamePadRequest` | Removes saved pairing data |

Mutation methods return `std::nullopt` on success or an `Error` on failure.
Transport adapters may execute them asynchronously even though the C++
interface itself is synchronous.

## Requests

```cpp
barista::api::StartSessionRequest start{
    .interfaceName = "wlan1",
    .mode = barista::api::SessionMode::Real,
};

barista::api::PairRequest pair;
pair.interfaceName = "wlan1";
pair.mode = barista::api::SessionMode::Real;
pair.code = {0, 1, 2, 3};
```

Session modes are serialized as:

| Enum | Wire name | Behavior |
| --- | --- | --- |
| `SessionMode::Real` | `real` | Screen, audio, and controller input through AppHook |
| `SessionMode::Controller` | `controller` | GamePad input exposed through the host virtual-controller path |

Use `SessionModeName()` and `ParseSessionMode()` at adapter boundaries rather
than duplicating string conversion.

### Validation

`ValidInterfaceName()` accepts 1–15 ASCII letters, digits, `_`, `-`, and `.`.
The name cannot start with `-` and cannot be `.` or `..`. A valid name is only
syntactically safe; the backend must still verify that it identifies an
eligible wireless adapter.

Pairing codes contain exactly four digits, each from `0` through `3`. Use
`ParsePairCode()` when receiving text and `PairCodeName()` when serializing the
four-byte representation.

MAC addresses and GamePad names currently have no portable validation helper.
Adapters should treat them as data, not shell fragments or paths.

## Errors

| Code | Meaning |
| --- | --- |
| `Unavailable` | The service or requested backend cannot be reached |
| `InvalidArgument` | A request failed validation |
| `Unauthorized` | The caller was not permitted to perform the operation |
| `Busy` | Another operation or incompatible session is active |
| `Unsupported` | The platform or backend does not provide the capability |
| `Failed` | The operation failed for another reason; inspect `message` |

An adapter should preserve the error category where its transport supports
structured errors. The current D-Bus adapter exposes named D-Bus errors, while
its status map contains only the human-readable error message.

## Ownership and lifecycle rules

- A session has one controlling caller. Other callers may inspect status but
  do not receive the private media endpoint.
- Only the owner may stop an active session through the current Linux service.
- Starting, pairing, stopping, or preparing can require host authorization.
- A caller should wait until `busy == false` before starting another mutation.
- `running` means the engine process exists; `gamePadConnected` separately
  reports whether the physical GamePad is connected.
- Pairing is not a separate long-lived service. A successful pair request moves
  into the requested running mode.

The normal state progression is:

```text
idle -> preparing -> starting -> runtime -> stopping -> idle
                    pairing --^

Any active phase may transition to failed.
```

Callers must use the returned phase and flags rather than assuming that an
accepted mutation has completed immediately.
