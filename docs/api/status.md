# Status Model

[`SessionStatus`](../../core/api/types.h) is the common snapshot returned by a
control backend. Defaults describe an unavailable, idle backend; adapters fill
the fields they can support.

## Top-level fields

| Field | Type | Meaning |
| --- | --- | --- |
| `apiVersion` | `uint32_t` | Contract version; currently `2` |
| `available` | `bool` | The control backend is reachable |
| `activating` | `bool` | An adapter is being activated but is not ready yet |
| `platform` | `string` | Backend platform identifier, currently `linux` for the installed service |
| `phase` | `SessionPhase` | Current lifecycle phase |
| `pairingStep` | `PairingStep` | Safe startup progress: `none`, `checking-adapter`, `setting-up-adapter`, or `creating-network`; only populated during startup |
| `mode` | optional `SessionMode` | Active/requested mode, absent while idle |
| `running` | `bool` | The session engine is running |
| `gamePadConnected` | `bool` | A physical GamePad is linked |
| `batteryPercent` | optional `uint8_t` | Reported battery percentage when known |
| `interfaceName` | `string` | Wireless interface assigned to the session |
| `ownedByCaller` | `bool` | The requesting control client owns this session |
| `busy` | `bool` | An authorization or stop operation is pending |
| `mediaEndpoint` | `string` | Caller-private media endpoint; empty when unavailable or not owned |
| `application` | `ConnectedApplication` | AppHook client metadata |
| `capabilities` | `Capabilities` | Features supported by this backend |
| `health` | `ServiceHealth` | Host dependency checks and migration warnings |
| `error` | optional `Error` | Most relevant current failure |

`available` and `activating` are especially useful to client adapters. For
example, the Qt D-Bus client sets `available` after a successful service call;
they are not keys returned by the Linux service itself.

## Phases

| Enum | Wire name | Meaning |
| --- | --- | --- |
| `Idle` | `idle` | No transition or session is active |
| `Preparing` | `preparing` | Host services or controller support are being prepared |
| `Pairing` | `pairing` | The engine is waiting for or pairing a GamePad |
| `Starting` | `starting` | A saved GamePad session is starting |
| `Runtime` | `runtime` | The session is operational |
| `Stopping` | `stopping` | Resources are being released |
| `Failed` | `failed` | The session could not continue |

Use `SessionPhaseName()` and `ParseSessionPhase()` when adapting a textual
transport. Unknown wire values should be treated as incompatible or failed,
not silently mapped to `idle`.

## Connected application

| Field | Meaning |
| --- | --- |
| `connected` | An AppHook client currently owns the connection |
| `name` | Process name reported by the local operating system |
| `pid` | Local process ID |
| `lastSeen` | Unix timestamp in seconds of the latest client activity |
| `connectedAt` | Unix timestamp in seconds when the client connected |
| `idleLogo` | Connector idle-art metadata when present |

These values describe a local process in the current AppHook adapter. A future
VM/network adapter should avoid pretending a guest process ID is a host PID;
it may leave unsupported fields empty or introduce versioned peer metadata.

## Capabilities

| Field | Meaning |
| --- | --- |
| `pairing` | The backend can pair a physical GamePad |
| `controller` | Virtual-controller output is available |
| `systemPreparation` | The backend can prepare required host services |
| `mediaStreaming` | The backend accepts application video/audio |
| `controllerSetup` | The installed helper can enable controller support |

Clients should gate controls on capabilities instead of branching only on
`platform`.

## Service health

| Field | Meaning |
| --- | --- |
| `networkManagerRunning` | NetworkManager is registered on the system bus |
| `authorizationRunning` | PolicyKit is registered on the system bus |
| `engineInstalled` | A trusted engine executable is installed |
| `hostapdInstalled` | A trusted hostapd executable is installed |
| `authorizationInstalled` | The authorization helper is securely installed |
| `legacySessionPresent` | The legacy `/tmp/drcd.sock` endpoint exists |
| `missingTools` | Required command-line tools not found on trusted paths |

Health fields are observations, not promises that the next operation will
succeed. Hardware support, driver state, authorization, and radio conditions
can still make an operation fail.

## Diagnostic errors

`Error::diagnosticCode` is a stable identifier suitable for support searches
and UI guidance. `Error::action` contains the recommended next step. The older
human-readable `message` remains available for context. Clients should display
the action when present and must not parse the message to make control-flow
decisions.
