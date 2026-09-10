# Barista

<p align="center"><img src="barista-logo.png" width="220" alt="Barista logo"></p>

Barista lets a Wii U GamePad act as a wireless second screen, controller, and
audio device for a desktop application. It owns pairing, the dedicated Wi-Fi
access point, low-latency media delivery, GamePad input, and a Qt 6 desktop
application. It is not affiliated with Nintendo.

## What it does

- Pair and reconnect a real Wii U GamePad from the desktop app.
- Show Barista's logo while no application is streaming.
- Run in **Screen + controller** mode: an application sends video/audio to the
  GamePad and receives its input.
- Run in **Controller-only** mode: expose standard buttons, sticks, triggers,
  and D-pad through Linux `uinput`.
- Keep the privileged radio/service work behind a D-Bus + polkit boundary; the
  Qt application runs as the regular desktop user.

The application connector is **AppHook**, internally nicknamed **MUG** (Media
User Gateway). It is a Linux-local Unix `SOCK_SEQPACKET` protocol, not an
emulator-specific integration. It carries RGB video, stereo PCM, and GamePad
input reports. Other applications can implement AppHook instead of embedding
any radio or pairing logic.

## Projects that work with Barista

These are experimental forks, not features of or endorsements by their
upstream projects.

- [BetaZay/Cemu](https://github.com/BetaZay/Cemu), branch
  `barista-connector-prototype` — Wii U GamePad video, audio, controls, and
  touchscreen integration.
- [BetaZay/Azahar](https://github.com/BetaZay/Azahar) — 3DS top/bottom-screen
  layouts, video, audio, controls, touchscreen, motion, and C-stick support.

The prototypes use Barista's default per-user session socket,
`/run/barista/media-<uid>.sock`, automatically. Other clients can use that
endpoint directly or set `BARISTA_MUG_SOCKET` as a development override.

## Wi-Fi requirements

The GamePad uses a dedicated **5 GHz** access point. Barista needs a Linux Wi-Fi
adapter and driver that can create a 5 GHz AP using `nl80211`/hostapd, remain
stable at the selected channel, and support the GamePad's required association
and encryption behavior. This is not ordinary home Wi-Fi: starting a session
temporarily takes over the selected adapter from NetworkManager.

Use wired Ethernet or a second Wi-Fi adapter if the machine needs Internet
access while Barista is active. Keep the GamePad close to the adapter while
testing; radio conditions still directly affect video quality and latency.

Development and physical GamePad streaming were tested with the Realtek
**RTL8852BE** (`rtw89_8852be`) on Linux. That proves this adapter/driver can
work; it is not a guarantee for every firmware, kernel, access-point channel,
or adapter. macOS and Windows currently provide the portable UI/core only—the
real GamePad radio backend is Linux-only.

The [TP-Link Nano AC600](https://www.amazon.com/dp/B07PB1X4CN) USB adapter
(`rtw_8821au`) is also a recommended tested option. It can take several
seconds to leave managed Wi-Fi mode and bring up the pairing access point;
wait for Barista to report that pairing is ready before using SYNC. It was
stable once the pairing AP was running in our testing.

For additional confirmed and incompatible hardware reports, see
[Vanilla Wii U's Wireless Compatibility wiki](https://github.com/vanilla-wiiu/vanilla/wiki/Wireless-Compatibility).

## Use

Install Barista, then open it normally from the desktop launcher. The system
service is activated on demand; use **Start** to authorize the Wi-Fi takeover.
Use **Pair GamePad** only when pairing is needed. Closing the window keeps it in
the tray by default.

The Advanced page displays the automatically managed AppHook endpoint. Clients
should use that default endpoint; the environment variable is only an optional
development override:

```sh
your-app
```

The endpoint exists only while a Screen + controller session is active. It is
owned and permissioned for the desktop user; applications must not run as root.

## Troubleshooting and support logs

Open **Advanced** when a session does not start, pair, connect, or stream
correctly. Barista shows a diagnostic code with a suggested next step. Use
**View log** to inspect an individual run, pairing-cycle, or maintenance log, **Open log
folder** to browse retained logs, or **Copy support report** to collect the
current system and session summary for a bug report.

Support logs are designed to be safe to share. They contain coded state changes,
versions, adapter driver information, and health checks. They exclude MAC and IP
addresses, SSIDs, pairing codes, credentials, usernames, and raw hostapd output.
Detailed private engine logs remain available to administrators for local
investigation and should be reviewed before being shared.

## Build and install

See [COMPILING.md](COMPILING.md) for dependencies, a development build, tests,
and system installation.

Developers integrating an application, control client, or future backend should
start with the [API documentation](docs/api/README.md).

## Status

Barista is active development software. The primary Linux pairing, streaming,
and AppHook paths have been exercised with real hardware, but adapter/driver
compatibility and media recovery still need broader hardware testing.

### What's working

- Pairing
- Basic A/V streaming
- Inputs from buttons and sticks
- AppHook for third-party apps

### Needs work

- Video still has artifacts and can get behind and stutter.
- Audio occasionally stutters.
- The GamePad still requests recovery much more often than it does with a real
  Wii U connection.
- Touch needs broader application and calibration testing.
- Gyro needs broader application and orientation testing.

### Not started

- Camera
- Microphone
- NFC
