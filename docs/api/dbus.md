# Linux D-Bus API

The installed Linux service exposes the control API on the system bus.

| Property | Value |
| --- | --- |
| Bus name | `org.barista.Service1` |
| Object path | `/org/barista/Service1` |
| Interface | `org.barista.Service1` |
| Authorization action | `org.barista.manage-session` |

D-Bus activation is supported, so clients should allow the bus to start the
service instead of launching it themselves. The GUI and connector applications
must remain unprivileged.

## Methods

| Method | Arguments | Reply |
| --- | --- | --- |
| `GetStatus` | none | Variant map described below |
| `GetDiagnostics` | none | Privacy-safe support report and retained support-log metadata |
| `SavedGamePads` | none | List of maps with `mac` and `name` |
| `StartSession` | `interface: string`, `mode: string` | Empty on completion; compatibility entry point without a country override |
| `StartSessionWithCountry` | `interface: string`, `mode: string`, `regulatoryCountry: string` | Empty on completion |
| `Pair` | `interface: string`, `code: string`, `mode: string` | Empty on completion; compatibility entry point without a country override |
| `PairWithCountry` | `interface: string`, `code: string`, `mode: string`, `regulatoryCountry: string` | Empty on completion |
| `StopSession` | none | Empty on completion |
| `PrepareSystem` | none | Empty on completion |
| `RenameGamePad` | `mac: string`, `name: string` | Empty |
| `RemoveGamePad` | `mac: string` | Empty |

The session, pairing, stop, and preparation methods use PolicyKit and may
hold the D-Bus reply while user authorization or setup completes. The bundled
Qt client allows 150 seconds for mutations and 25 seconds for reads; those are
client policy, not wire-level guarantees.

`RenameGamePad` and `RemoveGamePad` currently return no structured persistence
result. Call `SavedGamePads` afterward if confirmation matters.

`regulatoryCountry` is empty or a two-letter country code. A configured code
allows the engine to temporarily replace an unset `00` wireless regulatory
domain; it never replaces another configured country. The engine restores its
previous value when the session stops unless the value changed externally.

## Status map

The D-Bus status reply maps to `SessionStatus` as follows. Consumers must ignore
unknown keys so fields can be added compatibly.

| D-Bus key | Shared field | D-Bus value |
| --- | --- | --- |
| `apiVersion` | `apiVersion` | Unsigned integer |
| `platform` | `platform` | String |
| `running` | `running` | Boolean |
| `phase` | `phase` | `idle`, `preparing`, `pairing`, `starting`, `runtime`, `stopping`, or `failed` |
| `connected` | `gamePadConnected` | Boolean |
| `mode` | `mode` | Empty, `real`, or `controller` |
| `interface` | `interfaceName` | String |
| `batteryAvailable` | Presence of `batteryPercent` | Boolean |
| `battery` | `batteryPercent` | Unsigned integer; meaningful only when available |
| `ownedByCaller` | `ownedByCaller` | Boolean |
| `busy` | `busy` | Boolean |
| `error` | `error.message` | String; empty when no current error |
| `errorCode` | `error.diagnosticCode` | Stable diagnostic identifier; empty when no current error |
| `errorAction` | `error.action` | Suggested recovery step; empty when no current error |
| `mediaEndpoint` | `mediaEndpoint` | String, disclosed only to the owning caller in real mode |
| `appConnected` | `application.connected` | Boolean |
| `appName` | `application.name` | String |
| `appPid` | `application.pid` | Unsigned integer |
| `appLastSeen` | `application.lastSeen` | Unsigned 64-bit Unix timestamp |
| `appConnectedAt` | `application.connectedAt` | Unsigned 64-bit Unix timestamp |
| `appIdleLogo` | `application.idleLogo` | String |
| `controllerSupported` | `capabilities.controller` | Boolean |
| `pairingSupported` | `capabilities.pairing` | Boolean |
| `setupSupported` | `capabilities.systemPreparation` | Boolean |
| `controllerSetupAvailable` | `capabilities.controllerSetup` | Boolean |
| `networkManagerRunning` | `health.networkManagerRunning` | Boolean |
| `polkitRunning` | `health.authorizationRunning` | Boolean |
| `engineInstalled` | `health.engineInstalled` | Boolean |
| `hostapdInstalled` | `health.hostapdInstalled` | Boolean |
| `authorizationInstalled` | `health.authorizationInstalled` | Boolean |
| `missingTools` | `health.missingTools` | List of strings |
| `legacySessionPresent` | `health.legacySessionPresent` | Boolean |

The Qt adapter currently derives `capabilities.mediaStreaming = true`; there is
no separate `mediaStreaming` key in the D-Bus map.

## Diagnostics map

`GetDiagnostics` is read-only and does not require PolicyKit authorization. It
returns these keys:

| Key | Value |
| --- | --- |
| `schemaVersion` | Support-report schema version |
| `report` | Bounded plain-text support report suitable for copying or saving |
| `logDirectory` | User-readable support-log directory |
| `logFiles` | Newest-first list of retained run and pairing-cycle log names |
| `latestLog` | Newest retained log name, or empty |
| `sessionId` | Correlation ID for the current or most recent service session |

The report and listed support logs are allowlisted outputs. They exclude MAC
and IP addresses, SSIDs, pairing codes, credentials, usernames, and raw hostapd
output. Private engine logs are not returned over D-Bus.

## Errors and authorization

The service currently uses these D-Bus error names:

| Error name | Meaning |
| --- | --- |
| `org.barista.Error.Invalid` | Invalid pairing code |
| `org.barista.Error.Busy` | Another authorized operation is pending |
| `org.barista.Error.Caller` | The unique caller or its UID could not be established |
| `org.barista.Error.Operation` | Authorization, setup, validation, ownership, or engine operation failed |

The system-bus policy permits clients to send requests, but privileged methods
independently verify the caller and authorize the PolicyKit action. The service
binds session ownership and the media socket to that caller's UID. It does not
pass pairing keys or privileged radio access to the GUI.

## Command-line inspection

These calls are useful during development:

```sh
busctl call org.barista.Service1 /org/barista/Service1 \
    org.barista.Service1 GetStatus

busctl call org.barista.Service1 /org/barista/Service1 \
    org.barista.Service1 GetDiagnostics

busctl call org.barista.Service1 /org/barista/Service1 \
    org.barista.Service1 SavedGamePads

busctl call org.barista.Service1 /org/barista/Service1 \
    org.barista.Service1 StartSessionWithCountry sss wlan1 real US

busctl call org.barista.Service1 /org/barista/Service1 \
    org.barista.Service1 PairWithCountry ssss wlan1 0123 real US
```

The last two calls can display an authorization prompt and alter the selected
wireless adapter's state.
