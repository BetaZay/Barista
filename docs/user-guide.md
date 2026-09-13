# Using Barista

[Back to README](../README.md)

Install Barista using the [build and installation guide](../COMPILING.md), then
open it from the desktop launcher as your normal user. The system service starts
on demand and handles privileged radio work. Do not run the GUI or connected
applications with `sudo` or `pkexec`.

## Before connecting

In **Settings → General**, select your Wi-Fi adapter, confirm the country matches
your physical location, and choose a play mode:

- **Screen + controller**: a compatible application sends video/audio to the
  GamePad and receives its input. Barista displays its logo while no application
  is streaming.
- **Controller-only**: exposes standard buttons, sticks, triggers, and D-pad
  through Linux `uinput`.

The selected adapter is dedicated to the GamePad during a session. Use Ethernet
or another adapter for Internet access. See [Wi-Fi compatibility](hardware.md).

## Pair a new GamePad

1. Turn on the GamePad and keep it near the adapter.
2. Open **GamePads → Pair a GamePad**, then select **Pair** and authorize the
   Wi-Fi takeover when prompted.
3. Wait while Barista checks the adapter and creates the network.
4. When **Pair now** and the four symbols appear, press SYNC on the GamePad and
   enter the symbols from left to right.

Each Barista launch generates a new pattern for new pairings. Existing GamePads
retain their saved credentials; a new pattern does not replace those credentials.

**Cancel**, Escape, or closing the pairing popup stops an in-progress pairing
session and releases the adapter. Closing the instructions without starting a
pair does not stop an existing connection.

## Reconnect and disconnect

Use **Connect GamePad** on Home to reconnect a saved GamePad. Home shows the
session, battery level when available, and connected application.

**Disconnect GamePad** ends the session and releases the adapter. Closing the
main window keeps Barista in the tray by default; change this in Settings.
**Quit Barista**, at the bottom of the sidebar or in the tray menu, stops the
session and exits.

## Compatible applications

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

Start a **Screen + controller** session, then launch the application normally.
The AppHook endpoint exists only during that session and is permissioned for
the desktop user. **Settings → Support** displays it.

AppHook (internally called MUG, Media User Gateway) is a Linux-local Unix
`SOCK_SEQPACKET` protocol carrying RGB video, stereo PCM, and GamePad input.
Applications can implement it without embedding radio or pairing logic; see
the [API documentation](api/README.md).

## Settings and support

Settings groups configuration in **General**, read-only connection details in
**Session info**, diagnostics in **Support**, and project information in **About**.
For failures or bug reports, see [troubleshooting and support logs](troubleshooting.md).
