# drcd

Privileged runtime daemon.

Current implementation:

- Unix socket control server (`/tmp/drcd.sock`).
- `ServerCore` state machine backed by `libdrc-host`.
- Linux backend that:
  - writes hostapd pairing/runtime configs and WPS credential blobs,
  - spawns `hostapd` for pairing/runtime AP phases,
  - arms `WPS_PIN` through hostapd control socket (`/var/run/hostapd/<iface>`),
  - temporarily marks the interface unmanaged through NetworkManager,
  - configures `192.168.1.10/24` with MTU 1800 and serves `192.168.1.11` by DHCP,
  - keeps cleanup local (hostapd stop + temp file removal).
- Commands: `status`, `pair-start`, `pair-complete`, `pair-stop`, `runtime-start`,
  `runtime-stop`, `set-connected`, `shutdown`.
- A `WPS-SUCCESS` event automatically replaces the pairing AP with the secured
  runtime AP. `pair-complete`/`runtime-start` remain manual fallbacks.

Notes:

- `iw` is used to normalize interface type and `nmcli` temporarily releases the
  interface when NetworkManager is present. NetworkManager ownership is restored
  when the session stops.
- Root/capability requirements still apply because AP mode is configured by `hostapd`.

Environment knobs:

- `DRCD_HOSTAPD_BIN` (default: drc-project's patched hostapd on Linux)
- `DRCD_USE_PKEXEC` (default: `false`)
- `DRCD_AP_CHANNEL` (default: `36`)
- `DRCD_WPS_PIN_TIMEOUT` (default: `600`, range: `1`–`3600`) lifetime in seconds for the active pairing PIN
- `DRCD_KEEP_FAILED_TEMP` (default: `false`) keep failed hostapd/temp files under `/tmp` for debugging
- `DRCD_LOG_STDERR` (default: `true`) enable daemon/backend stderr logging
- `DRCD_LOG_HOSTAPD_RAW` (default: `false`) log every hostapd line (very noisy); default logs only key AP/WPS/STA/failure events
- `DRCD_PAIR_IGNORE_BROADCAST_PROBES` (default: `false`) set pairing AP to hidden-SSID behavior (`ignore_broadcast_ssid=1`) to ignore wildcard probe requests
- `DRCD_PAIR_CHANNEL_SWEEP` (default: `true`) rotate the live pairing AP across the channel plan until authentication, association, or WPS activity is detected (default plan: `36,40,44,48,149,153,157,161,165`)
- `DRCD_PAIR_CHANNEL_LIST` (default: empty) explicit channel attempt order (for example: `44,36`)
- `DRCD_PAIR_CHANNEL_DWELL_MS` (default: `20000`, range: `1000`–`60000`) live pairing-AP dwell time per channel; the daemon logs a three-second countdown before switching

Default concise logging still reports probe discovery as:
- `probe-seen: sa=<mac> local-admin=<0|1>`
with per-MAC rate limiting to avoid log spam.

During pairing, non-locally-administered probe MACs are also used for targeted `WPS_PIN <mac> ...` retries.

Roadmap:

- Replace remaining hostapd process coupling with a tighter native control surface.
- Add policy/install tooling for least-privilege deployment.
## Media stream test

`drcd` owns the Wi-Fi AP, pairing/runtime transition, DHCP lease, and Wii U
GamePad application-protocol sockets. The optional media test exercises that
complete path without Cemu.

To test the complete audio/video path without Cemu, pass any FFmpeg-readable
media file. Playback starts only after the GamePad has obtained its lease and
valid DRC protocol traffic is received:

```sh
sudo ./build/drcd/drcd --np --play ./test1.mkv
```

The media is scaled to 864x480, converted to 59.94 Hz YUV420P and encoded with
the bundled DRC-patched x264. Audio is converted to stereo 48 kHz signed
16-bit PCM. The file loops until the daemon exits.

To isolate video packet delivery from file decoding, picture complexity, and
audio traffic, stream a built-in static black frame at 59.94 Hz:

```sh
sudo ./build/drcd/drcd --np --black
```
