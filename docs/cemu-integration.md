# Cemu through drcd (Linux)

Cemu is an unprivileged media/input client. Only drcd owns pairing credentials,
hostapd, DHCP, GamePad UDP ports, AP TSF reads, H.264 encoding and keepalive.
No emulator-side helper, polkit request or wireless interface management remains.
The local `WiiUGamePad` input API is retained but reads reports from drcd only.

## Run

Build both projects:

```sh
cmake --build drc-project/build --parallel 4
cmake --build Cemu/build --parallel 4
```

In a terminal, from `drc-project`, start the already-paired session:

```sh
sudo env DRCD_CEMU_SOCKET=/tmp/drcd-media.sock ./scripts/capture-drcd-session.sh /tmp/drcd-cemu.pcap --np
```

Do not supply `--play` or `--black`. Leave the fast-encode, generated-A/V and
reference-header experiments off. Do not run the register probe concurrently.

Current defaults: QP32, video timestamped just before send at clock-6250us,
init bit on every IDR, and five chunks starting at 0/3/6/9/11ms per frame.
Packets within a chunk remain contiguous. Post-IDR extra pause is off.
The verified RTL8852BE driver automatically selects the actual AP TSF clock;
failure to read that clock fails startup rather than silently using RX time.
Other adapters retain their existing clock path; this auto-selection is not
a claim of support for other hardware register layouts.

Only `DRCD_CEMU_SOCKET` selects the Cemu source; it is not made global because
that would interfere with file/generated-video modes. For isolated rollback,
`DRCD_CHUNK_PACING=0` restores whole-frame bursts. `DRCD_SEND_TIME_VIDEO=0`
and `DRCD_IDR_INIT=0` disable their respective defaults. The existing explicit
QP overrides still work. Clear old shell overrides before testing defaults.
Chunk pacing still needs physical acceptance. Encoding now overlaps paced
transmission on a separate serial sender. There is one sending frame and at
most one next frame being encoded/waiting for handoff; encoded frames are never
dropped to catch up. Expensive encodes can still cause overruns; the target
remains 59.94Hz, not a guarantee of achieved frame rate.

In another terminal, from the workspace root, run Cemu **without sudo**:

```sh
env CEMU_DRCD_SOCKET=/tmp/drcd-media.sock ./Cemu/bin/Cemu_debug
```

Either process can start first; the client reconnects. Turn on the paired pad.
Cemu sends its existing icon asset centered on a dark 864x480 idle image with
silent PCM. Launch a game to send the game's **DRC render target**, not TV
output or desktop/window pixels. The separate GamePad window need not be open,
and TV/GamePad window swapping does not change the physical pad's source.
Stopping a title switches back to the logo. Closing Cemu leaves the last idle
logo on drcd, with silence and no stale controller input.

For physical controls, select emulated controller **Wii U GamePad**, add input
API **WiiUGamePad**, select its controller and use the default mapping.
Buttons/sticks are bridged; touch/motion/rumble are not implemented in this
replacement adapter yet. GamePad 48kHz PCM is tapped before the host audio
device, so it does not depend on hearing Cemu through desktop speakers.

Cemu's software keyboard is a special case: while active, its main-window UI
is sent on a dark background instead of the guest DRC texture. Keep the main
window rendering (not minimized). Use D-pad/A to select keys, B to backspace
and Plus to confirm; physical touchscreen typing is not implemented. Normal
GamePad game output resumes when the keyboard closes. This mirrors Cemu's
keyboard, not Nintendo's original keyboard artwork. Physical validation of
this keyboard path is still pending.

Expected drcd log messages include `Cemu media bridge listening`,
`Cemu IPC client connected`, and `Cemu source: game` / `Cemu source: idle logo`.
The daemon creates a mode-0600 socket owned by the invoking `SUDO_UID` (or its
own UID when not run via sudo). Peer credentials must match that UID or root.
Do not run multiple daemons or different media clients on this endpoint.
An existing socket is never automatically removed: inspect the owning process
before removing a stale socket after a crash. Normal shutdown removes only the
socket inode created by that daemon.

## Implementation and limits

- `libdrc-ipc` is a small local transport library, not libdrc's Wi-Fi stack.
- Linux Unix `SOCK_SEQPACKET`; fixed versioned 16-byte headers; I420 frames
  split into at most 16KiB payloads. Only contiguous complete frames publish.
- RGB scaling and BT.601 limited-range conversion run in the client worker.
  Pending video is a single replaceable frame; no unbounded frame queue.
- PCM buffers are limited to 100ms; overflow drops old samples and underflow
  sends silence. Game audio and video share the daemon's AP clock but there is
  no emulator presentation-timestamp synchronization yet.
- Idle image is retained separately; active/idle heartbeat is sent every 100ms.
  Input expires after 500ms, and dead clients are disconnected after 2s.
- GamePad input is forwarded by a separate 1ms-poll worker, not by the video
  encoder loop. Main button bytes are big-endian; stick fields are little-endian.
- Encoding and the working radio-clock/packet settings are unchanged. File
  playback and generated test modes remain available without the bridge option.
- OpenGL/Vulkan capture currently reuses synchronous GPU readback. It does not
  consume screenshot requests or save screenshots, but it can stall the GPU.
  This is a functional first integration, **not a validated low-latency path**.
  RGB uses the existing screenshot path's color conversion. Aspect ratio is
  scaled to the fixed DRC surface; graphic-pack output shaders/host overlays
  are not captured, except the software-keyboard overlay described above.
  That overlay is CPU-rasterized from existing ImGui draw data and adds CPU
  work while typing; it does not execute input callbacks a second time.
  Metal streaming is not implemented.
- Unit tests cover conversion, complete frame transfer, idle/game switching,
  audio byte order, input, stale input, reconnect, malformed packets and socket
  ownership cleanup. Physical Cemu logo/game rendering and improved button
  input have been confirmed by the user. A separate `CemuDrcdOverlayTests`
  target tests keyboard rasterization and legacy navigation press/release.

## Old integration backup

The removed pairing GUI, provider transport, hostapd source/helpers and tracked
source snapshots are recoverable from
`checkpoints/cemu-before-drcd-20260904.tar.gz` in drc-project. A copy is also at
`/tmp/cemu-drc-migration-PoxSUN/old-integration.tar.gz`. Removed source directories
and helper executables were also moved into that directory. This is a temporary
backup; the source archive in checkpoints is durable. Previously installed system polkit
policies were not modified or removed, and are no longer used by Cemu.
