# Media and Input API

The portable media contracts are
[`MediaProducer` and `InputConsumer`](../../core/api/media.h). The installed
Linux implementation currently exposes equivalent behavior through AppHook,
a local Unix socket connector declared in
[`app_hook.h`](../../core/api/app_hook.h).

## Portable frame contracts

### Video

`VideoFrame` contains a sequence number, a monotonic capture timestamp, width,
height, planar I420 bytes, and an `idle` flag. An implementation must retain at
most one frame waiting for encoding: a newer frame replaces an older pending
frame. This latest-frame-wins rule prevents congestion from turning into
ever-growing display latency.

For an I420 frame, `i420.size()` is expected to be `width * height * 3 / 2`,
with an 8-bit full-resolution Y plane followed by quarter-resolution U and V
planes. Dimensions used for chroma subsampling should be even.

### Audio

`AudioFrame::stereoPcm` is interleaved signed 16-bit stereo PCM. The default
sample rate is 48 kHz. Sequence and monotonic capture time allow an adapter to
detect discontinuities without relying on wall-clock time.

### Input

`InputReport` contains one 128-byte raw GamePad report plus its sequence and
monotonic receive time. Decoding into desktop controller events belongs above
the transport boundary.

## AppHook quick start

An application connector uses the client role (`false`) and connects to the
endpoint returned in `SessionStatus::mediaEndpoint`. In an installed real-mode
session this is normally `/run/barista/media-<uid>.sock`.

```cpp
#include "api/app_hook.h"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

std::string endpoint = "/run/barista/media-" + std::to_string(getuid()) + ".sock";
if (const char* overridePath = std::getenv("BARISTA_MUG_SOCKET"))
    endpoint = overridePath;

barista::api::AppHook hook(false);
std::string error;
if (!hook.start(endpoint, error))
    throw std::runtime_error(error);

hook.set_active(true);

// rgb24 contains width * height * 3 bytes. Ownership moves to AppHook.
hook.submit_rgb(std::move(rgb24), width, height);

// Interleaved signed 16-bit stereo at 48 kHz.
hook.submit_pcm(stereoSamples);

std::array<uint8_t, 128> report{};
if (hook.read_input(report))
{
    // Decode a fresh GamePad input report.
}
```

The example requires the usual headers for `getuid`, exceptions, and the
application's own `rgb24`, dimensions, and audio storage. Production clients
should obtain the endpoint through the control API; `BARISTA_MUG_SOCKET` is a
development override.

A successful `start()` means the connector worker started; use `connected()`
when the application needs to know whether a peer is currently linked. The
client reconnects in the background when the service endpoint is temporarily
unavailable.

## AppHook formats and limits

| Item | Contract |
| --- | --- |
| Transport | Linux-local Unix `SOCK_SEQPACKET` |
| Video input | Packed RGB24, exact `width * height * 3` bytes |
| Accepted source size | Non-zero width/height, each at most 8192 |
| Engine video frame | Fixed 864x480 I420 (`622080` bytes) |
| Conversion | RGB24 is scaled and converted to limited-range BT.601 I420 |
| Audio | Interleaved signed 16-bit stereo, 48 kHz |
| Audio bound | At most 9,600 samples, or 100 ms of stereo audio |
| Input | Exactly 128 bytes; reads expire after 500 ms |
| Video backpressure | Non-blocking submission; a busy converter may drop the new call |
| Connection count | One connector client per endpoint |

Call `set_active(true)` while the application is actively presenting content.
The client emits a heartbeat every 100 ms. The server treats activity as stale
after one second without a heartbeat and closes an unresponsive connection
after two seconds without packets.

`submit_rgb(..., idle = true)` supplies connector-owned idle art. It takes
precedence over the server fallback while that connector is present. Passing an
empty span to `set_idle_frame()` clears server fallback art; a non-empty fallback
must be exactly one 864x480 I420 frame.

## Local protocol notes

Applications should prefer the `AppHook` class instead of constructing packets.
For maintainers of an external connector, the current MUG1 packet is a
little-endian 16-byte header followed by at most 16,384 payload bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | `uint32` | Magic `0x3147554d` (`MUG1`) |
| 4 | `uint32` | Type: video `1`, idle `2`, active `3`, PCM `4`, input `5`, reject `6` |
| 8 | `uint32` | Frame/message ID |
| 12 | `uint32` | Byte offset within a chunked frame |

MUG1 is host-local and trusts Unix peer credentials and filesystem permissions;
it has no network authentication, negotiation, or encryption and must not be
exposed over TCP. The server accepts the configured desktop UID (or root), sets
the socket mode to `0600`, and rejects duplicate clients. Companion `.lock` and
`.idle.i420` files are implementation metadata, not separate public APIs.
