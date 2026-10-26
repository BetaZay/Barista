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
- **Desktop**: mirrors a monitor or application window and uses the sticks and
  buttons as a mouse and keyboard.
- **Game mirroring**: mirrors a monitor or game window and provides standard
  controller input for Steam and other PC games without game integration.

The adapter list refreshes automatically while idle; use **Refresh** to check
immediately after plugging in a USB adapter. A missing saved adapter is marked
unavailable. Select an available adapter before connecting or pairing.

The selected adapter is dedicated to the GamePad during a session. Use Ethernet
or another adapter for Internet access. See [Wi-Fi compatibility](hardware.md).

## Terminal commands

Use `barista` with an action to control the installed Linux service without
opening a window. `barista-cli` accepts the same options; `barista` without
options still opens the desktop app. Run these commands as your normal user.
Polkit handles authorization for pairing, starting, stopping, and system setup.

```sh
barista --list
barista --interfaces
barista --pair --interface wlan0 --country US
barista --start --interface wlan0
barista --start --interface wlan0 --mode controller
barista --status
barista --stop
barista --diagnostics
barista --help
```

`--list` shows wireless adapters and saved GamePads. `--interfaces` lists only
local adapters and works without the service. If you omit `--interface`, Barista
uses the GUI's saved adapter when available, or the sole wireless adapter.
With several adapters and no usable saved choice, specify one explicitly.
The service checks whether the selected adapter supports the session.

`--pair` generates a random four-symbol pattern. Wait for **Pair now** in the
terminal, press SYNC on the GamePad, and enter the displayed symbols from left
to right. To choose a pattern, add `--code 0123`: `0` is Spade, `1` Heart,
`2` Diamond, and `3` Club. Successful pairing continues into a running session.

`--start` reconnects using saved pairing data. The default mode is `real`, for
applications using AppHook; `--mode controller` provides a virtual controller.
Use the GUI for desktop and game mirroring, which require its capture controls.
`--country` is optional and must match your physical location; it is passed
explicitly and does not change the GUI's saved country setting.

Pairing and starting keep the CLI in the foreground so it remains the session
owner. Keep that terminal open while playing. Press Ctrl+C to stop, or run
`barista --stop` from another terminal in the same desktop login. `--stop`
requests shutdown through the owning CLI; use the GUI to disconnect a
GUI-owned session. Exiting the controlling process also releases its session.
Read-only status and listing commands do not take ownership.

`--prepare` prepares the host facilities without starting a session.
`--diagnostics` prints the service's shareable support report. Exit codes are
`0` for success, `1` for a service or session failure, and `2` for invalid
arguments. A successful `--stop` means the owner accepted the stop request;
the owning terminal reports any shutdown failure.

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

## Game mirroring mode

Select **Game mirroring** in **Settings → General** before connecting. When the
GamePad connects, share the monitor or window showing your Steam game or other
PC game. Games receive the sticks, D-pad, triggers and buttons through the
**Barista Wii U GamePad** virtual controller. No AppHook integration or special
launch options are needed in the game; Barista uses its internal media bridge
for the captured picture. Steam Input can map the controller for your game.

Keep Barista running while playing. Stopping sharing, disconnecting the pad,
or losing the input bridge releases held controls. Right stick click remains
a controller button rather than opening the desktop keyboard. This mode shares
video only; system audio, touch, motion and rumble are currently unavailable.

## Desktop mode

1. Select **Desktop** in **Settings → General** before connecting.
2. Use **Connect GamePad**, then turn on your paired GamePad.
3. On Wayland, the system sharing picker opens when the GamePad connects.
   Select one monitor or application window and approve sharing.
   On X11, select the source in Settings before connecting.
4. Use **Choose monitor or window…** on Wayland to change the source or retry
   a cancelled picker. On X11, select a source and use **Share selected source**.
   **Stop sharing** ends capture and releases desktop input while
   keeping the GamePad connected.

| GamePad control | Desktop action |
| --- | --- |
| Left stick | Move cursor; small movements are slower |
| Right stick | Vertical and horizontal scrolling |
| A or ZR | Left mouse button; hold to drag |
| B or ZL | Right mouse button |
| Left stick click | Middle mouse button |
| Right stick click | Open the GamePad touch keyboard |
| D-pad | Arrow keys |
| Plus / Minus | Enter / Escape |
| Y / X | Tab / Backspace |

Input controls the desktop cursor and focused application. When sharing a
single window, click it to give it keyboard focus. The source is scaled to fit
the GamePad display with black bars as needed. Static scenes continue streaming.
On X11, keep the selected window visible: overlapping windows can appear in
the capture, and minimizing or closing the source stops sharing.
Click **GamePad keyboard** in Barista or press the right stick to enter text on
its built-in keyboard. Tap keys, or use D-pad + A; B deletes, X toggles Shift,
Plus submits and HOME cancels. Submitted text is typed into the focused desktop
application. This first desktop typing implementation accepts printable ASCII
and assumes a US host keyboard layout on both X11 and Wayland. Characters are
paced at one per 8-ms input tick, with each key and Shift released after use;
stop/disconnect/stale input cancels pending typing. Applications can instead
use [AppHook text prompts](api/media.md#gamepad-software-keyboard) for direct
UTF-8 results, initial text, length limits and password masking.

![GamePad keyboard in the custom Home Menu style](screenshots/gamepad-keyboard.png)

This initial desktop mode shares video; system audio capture and touch mouse
input outside the keyboard are not implemented.

Wayland requires PipeWire and a working ScreenCast portal backend supplied by
your desktop, including when Barista runs through XWayland. X11 offers monitor
and application-window selection directly, including on Qt 6.4 builds. Both the GUI and the system service must be built from a version
that supports desktop mode. Capture runs as your desktop user. The service
creates the mouse/keyboard device and accepts input only from the client that
owns the session; stale input and disconnects release held buttons and keys.

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

Connection errors offer **View details** to open Support directly and **Retry**
when another attempt is possible. Retrying a GamePad connection still asks for
Wi-Fi takeover confirmation. In Support, copied and saved reports include the
selected sanitized session log by default; select the failed session's log or
uncheck **Include selected session log** for a report of the current state only.
