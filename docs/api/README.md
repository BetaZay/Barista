# Barista API

Barista's API separates application-facing control and media from the radio,
pairing, and encoder implementation. Portable contracts live in
[`core/api`](../../core/api); platform transports adapt those contracts without
becoming the source of truth.

## Start here

- [Control API](control.md) describes sessions, pairing, saved GamePads, errors,
  and request validation.
- [Status model](status.md) lists every `SessionStatus` field and the session
  lifecycle.
- [Linux D-Bus API](dbus.md) documents the installed system-service adapter.
- [Media and input](media.md) covers the portable frame contracts and the
  current Linux-local AppHook connector.
- [Transports and VM direction](transports.md) states what exists today and how
  a bundled VM or remote backend should fit without duplicating the API.

## Architecture

```text
desktop application / Barista GUI
                 |
       barista::api contracts
       /                   \
Linux D-Bus control    AppHook media/input
       \                   /
         service + DRH engine
                  |
            Wii U GamePad
```

The portable API has three boundaries:

| Boundary | Header | Responsibility |
| --- | --- | --- |
| Control | [`service.h`](../../core/api/service.h) | Session lifecycle, pairing, preparation, and saved GamePads |
| Data model | [`types.h`](../../core/api/types.h) | Requests, status, errors, media frames, and input reports |
| Media | [`media.h`](../../core/api/media.h) | Video/audio submission and input consumption |

`barista::api::ApiVersion` is currently `2`. Consumers should reject a higher
contract version they cannot understand and tolerate newly added optional
status fields in transport adapters.

## Using the C++ contracts

From another target in this repository:

```cmake
target_link_libraries(my_target PRIVATE barista::api)
```

```cpp
#include "api/service.h"
#include "api/media.h"
```

`barista::api` is an interface target, so using the data model does not pull in
Qt, D-Bus, Linux radio code, or the encoder. The present `AppHook`
implementation is built in the transitional `drc_ipc` target and is Linux-only;
its public class is declared in [`app_hook.h`](../../core/api/app_hook.h).

## Compatibility levels

| Surface | Current role | Compatibility expectation |
| --- | --- | --- |
| `barista::api` C++ types | Canonical application contract | Evolve deliberately with `ApiVersion` |
| `org.barista.Service1` | Installed Linux control adapter | Supported local API; map keys may be added |
| AppHook/MUG1 | Installed Linux-local media adapter | Supported by current connector clients, but not a portable network protocol |
| `/tmp/drcd.sock` control | Legacy compatibility | Internal and scheduled for replacement |
| VM/TCP transport | Planned | Must adapt the same control/media contracts; no endpoint or port exists yet |

The abstract `SessionService`, `MediaProducer`, and `InputConsumer` interfaces
define the intended boundary. The current Qt service does not yet implement
`SessionService` directly; it maps the shared types to D-Bus while that cleanup
continues.
