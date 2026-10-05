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
| 4 | `uint32` | Type: video `1`, idle `2`, active `3`, PCM `4`, input `5`, reject `6`, rumble `7`; optional keyboard `8`–`10` below |
| 8 | `uint32` | Frame/message ID |
| 12 | `uint32` | Byte offset within a chunked frame |

MUG1 is host-local and trusts Unix peer credentials and filesystem permissions;
it has no network authentication, negotiation, or encryption and must not be
exposed over TCP. The server accepts the configured desktop UID (or root), sets
the socket mode to `0600`, and rejects duplicate clients. Companion `.lock` and
`.idle.i420` files are implementation metadata, not separate public APIs.

## GamePad software keyboard

The streaming engine provides a touch keyboard in the custom Home Menu style.
A connector can use it for a game or application text prompt instead of drawing
its own keyboard. Continue submitting video and heartbeats while the prompt is
open; Barista draws the keyboard over that stream and consumes GamePad buttons,
sticks and touch until the prompt closes. Desktop mode uses the same interface.

```cpp
barista::api::KeyboardRequest request{
    .id = 1, // Unique among this connection's outstanding prompts.
    .title = "Player name",
    .initialText = "Link",
    .maxCharacters = 16,
    .password = false,
};
if (!hook.request_keyboard(request))
{
    // Disconnected, invalid request, or outgoing queue full: use your fallback.
}

barista::api::KeyboardResult result;
while (hook.read_keyboard_result(result))
{
    if (result.id != request.id)
        continue;
    if (result.outcome == barista::api::KeyboardOutcome::Submitted)
    {
        // Apply result.text (UTF-8) on the application's UI/emulation thread.
    }
    else if (result.outcome == barista::api::KeyboardOutcome::Cancelled)
    {
        // Close the prompt without changing the application's original text.
    }
    else
    {
        // Busy: another prompt is open. Keep the application's fallback available.
    }
}
// If the application dismisses its prompt:
hook.cancel_keyboard(request.id);
```

There is one visible prompt at a time. A busy response also covers an engine
started with its GamePad overlays disabled. Titles are limited to 256 UTF-8 bytes;
initial and submitted text are limited to 4,096 UTF-8 bytes. `maxCharacters`
accepts 1–1,024 Unicode scalars, not UTF-16 units or grapheme clusters. Invalid
UTF-8, embedded controls and initial text over the requested limit are rejected.
The first key layout enters printable English ASCII, with sticky Shift,
numbers, punctuation and a symbol page; valid Unicode initial text is preserved
and Backspace removes one complete scalar. Password prompts display masking
characters. Cancellation and busy responses contain no text. AppHook queues
are bounded to 16 messages in each direction.

Tap a key, or use the D-pad to select and A to type. B deletes, X toggles Shift,
Plus submits and HOME cancels. Cancel and Done are also touch buttons. Held
buttons and a closing touch remain consumed until released. Disconnecting
closes the prompt; requests and results are discarded across reconnects, so a
connector must reissue a still-needed prompt on the new connection. The server
ties each result to the requesting connection's internal revision.

The keyboard extension is opt-in. Existing connectors send and receive the
same media/input packets. New keyboard calls require an engine implementing
these messages; MUG1 has no capability negotiation, so requesting a keyboard
from an older engine can disconnect the connector. Keep the normal keyboard
as a fallback when supporting older Barista installations.

### External connector wire format

The existing 16-byte MUG1 header remains unchanged. All keyboard messages use
header message ID = prompt ID (nonzero), byte offset = 0, and one packet:

| Type | Direction | Payload |
| --- | --- | --- |
| `8` | Client → engine | LE `uint32 maxCharacters`, LE `uint32 flags` (bit 0: password), LE `uint32 titleByteLength`, title UTF-8 bytes, initial-text UTF-8 bytes |
| `9` | Client → engine | Empty payload; cancel the header's prompt ID |
| `10` | Engine → client | LE `uint32 outcome` (`0` submitted, `1` cancelled, `2` busy), followed by UTF-8 text; empty text for cancelled/busy |

Unknown flags, malformed lengths, invalid text or overflowing incoming queues
close the connection. This endpoint remains local and restricted to the session
owner; keyboard messages grant no radio or arbitrary system-event access.

### Cemu integration points

The neighboring Cemu fork already has `src/Common/BaristaAppHook.{h,cpp}` and
an emulated keyboard in `src/Cafe/OS/libs/swkbd/swkbd.cpp`. To route its game
prompts through this API, extend that connector with packet types 8–10 and a
thread-safe result queue. Request a keyboard when `SwkbdAppearInputForm` opens
an input form, converting the initial UTF-16 string and character limit to
this UTF-8 contract. Poll on Cemu's emulation/UI thread, convert a submitted
result back to its form buffer and follow its existing confirmation path.
Cancel when `SwkbdDisappearInputForm` closes the form; retain Cemu's current
keyboard when disconnected, busy or unavailable. Its keyboard-only API uses
per-key callbacks and needs a separate adapter; a final-string response is
suited to input forms. This change supplies the Barista interface; the Cemu
fork and the existing Cemu 2.8 AppImage are not patched by this feature.
