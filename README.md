# drc-project

Split Wii U GamePad host stack with three components:

- `libdrc-host/`: reusable core library (pairing/state/protocol helpers).
- `drcd/`: privileged daemon for AP lifecycle, hostapd, DHCP, and policy boundary.
- `drcctl/`: CLI client for control, status, and diagnostics.

## Quick Start

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Linux, this also builds the patched hostapd owned by this repository at
`build/third_party/hostapd-drc/bin/drc-hostapd`. `drcd` uses that binary by
default, so no Cemu checkout or `DRCD_HOSTAPD_BIN` override is required.

Run the daemon in automatic mode (the default) with the AP identity and sync
pattern you want to advertise:

```sh
sudo ./build/drcd/drcd --interface wlan0 --ap-mac 40:d2:8a:bf:fc:a8 --pair-code 2220
```

It first checks for an already paired GamePad for 5 seconds, advertises the
pairing network for 20 seconds, and repeats until a GamePad connects. After a
successful WPS exchange it saves the runtime credential in
`/var/lib/drcd/credentials.conf`, suppresses pairing for 60 seconds while the
GamePad restarts, and keeps offering only the saved runtime network during that
grace period. Later runs automatically reuse its AP MAC.
The concise terminal status is backed by a complete log at `/tmp/drcd.log`,
which is replaced once when `drcd` starts and retained across every phase in
that run. Set `DRCD_LOG_STDERR=1` for verbose terminal diagnostics, or pass
`--np` to keep checking the saved runtime network without entering pairing
mode. Pass `--manual` to retain explicit `drcctl pair-start` operation.

To passively locate traffic transmitted by a GamePad without creating an
access point, run:

```sh
sudo ./scripts/watch-gamepad-probes.sh wlan0 40:d2:8a:ab:90:00
```

The watcher switches the interface to monitor mode, revisits all non-DFS Wii U
5 GHz candidate channels every 2.25 seconds, captures every frame whose
transmitter is the requested GamePad, writes a radiotap pcap under `/tmp`, and
restores the previous interface type when stopped with Ctrl-C.

To capture a complete real Wii U-to-GamePad session for packet timing and
802.11 delivery comparison, use:

```sh
sudo ./scripts/capture-wiiu-session.sh wlan0 40:d2:8a:ab:90:00 auto
```

Auto mode discovers and prints the active channel. For the best trace, stop it
after discovery and rerun with that fixed channel before powering on the
GamePad. The saved pcap is deliberately unfiltered so that 802.11 ACK and
BlockAck frames are retained; the console's runtime data payload remains
encrypted unless its separate runtime key is available.

## Implemented

- `libdrc-host` exposes:
  - MAC address parse/format helpers.
  - Pairing symbol mapping (`♠=0, ♥=1, ♦=2, ♣=3`), PIN byte conversion, and pairing SSID generation.
  - A small pairing/runtime state machine.
- `drcd` exposes a Unix socket control API on `/tmp/drcd.sock`.
- `drcd` Linux backend now performs real AP lifecycle steps with hostapd:
  - pairing config + runtime config generation
  - WPS credential blob generation
  - hostapd process lifecycle + control-socket `WPS_PIN`
  - temporary NetworkManager exclusion while the AP owns the interface
  - `192.168.1.10/24`, MTU 1800, and a DHCP lease at `192.168.1.11`
  - deterministic cleanup on stop/shutdown
- `drcctl` can send control commands to `drcd`.
- Unit tests:
  - `drc_host_tests`
  - `drcd_core_tests`
  - `drcctl_core_tests`

Protocol reference: `docs/control-protocol.md`.

## Roadmap

1. Add native AP/interface orchestration to reduce external process coupling further.
2. Add integration tests that exercise `drcctl <-> drcd` over a real socket outside restricted sandboxes.
