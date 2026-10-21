# One-command GamePad experiments

Run `barista-capture` as your desktop user. Leave the GamePad off until the
command prints `READY`, then turn it on and launch Cemu and your game. By
default it uses `wlan0` for Barista and `wlan1` as an independent monitor radio.
It starts a real-mode session if none exists, or attaches to an existing
real-mode session on the requested adapter. It keeps its D-Bus connection alive
so a session it starts remains owned by your account throughout the test.

```sh
barista-capture --seconds 60
# Other adapters or a longer sample:
barista-capture --ap wlan0 --radio wlan1 --seconds 120
```

To build Barista and launch Cemu automatically **after** the service
confirms the GamePad connection, run this from the checkout:

```sh
python3 scripts/run-gamepad-experiment.py \
  --build-dir build-gamepad-release \
  --cemu /home/isaiah/Downloads/Cemu-2.8-x86_64.AppImage \
  --game '/mnt/Data/Emulation/roms/wiiu/roms/The Legend of Zelda - The Wind Waker HD (USA).wua' \
  --seconds 90
```

Close existing Cemu instances before starting this command. It launches the
test without Barista’s frontend; add `--show-ui` to open that window. It launches the
specified AppImage with `-g` and the per-user Barista socket, and leaves Cemu
running after the capture ends. Experiment GUI/Cemu/capture logs are kept in
the printed `/tmp/barista-experiment-*` directory. The script and desktop
applications run as your ordinary user; the installed capture helper alone
runs privileged.
At `READY` it emits a terminal bell, including to an ancestor's interactive
terminal when tool output is piped. Turn the GamePad on at that prompt.

Use a Release engine for hardware video tests. Debug builds introduced large
encoding delays on this workstation. Configure the selected tree with
`cmake -S . -B build-gamepad-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr`,
build it, and run `ctest --test-dir build-gamepad-release --output-on-failure`
before installing its engine. The runner builds the selected tree but does
**not** install it; the service runs the installed engine. Verify that engine
matches the tested binary. Omitting `--build-dir` uses the development `build/`
directory. Cemu may close after `ANALYZING` begins without interrupting analysis.

Recording begins before the `READY` prompt to retain association/EAPOL. The
helper waits up to 120 seconds for GamePad data, then records for 10–300 seconds.
For an existing session, power-cycle the GamePad after `READY` to include a fresh
handshake. The monitor follows the AP's actual channel; both adapters must be
on different PHYs. The command stops only a session it started. Cemu stays an
ordinary desktop application and is launched separately.

The `RESULT` line gives a private, timestamped directory under
`/var/lib/barista-captures`. It contains:

| File | Purpose |
| --- | --- |
| `radio.pcap` | Unfiltered radiotap capture, including EAPOL and Block ACKs |
| `host-ip.pcap` | Simultaneous AP-side plaintext DRC UDP capture |
| `decrypted-radio.pcap`, `decrypted-ip.pcap` | Authenticated radio/IP companions |
| `delivery.json` | Host/radio matching, complete frames, Block ACK evidence, requests |
| `summary.json` | Main delivery, recovery and timing measurements for comparing runs |
| `format-clock.json` | Format freshness, checksums, format-to-video lead |
| `host-media.json`, `wifi-rates.json` | Protocol structure/cadence and PHY/retry observations |
| `session.json` | Adapters, channel, capture interval and completion status |
| `service-diagnostics.txt` | Service's sanitized media/transport diagnostic report |
| `*-tcpdump.log` | Capture errors and kernel/drop counters |

Raw/decrypted files are private to the invoking user. The saved WPA credential
is read in the privileged helper and never copied to the results or printed.
Analysis uses the root-owned installed copies of the existing EEPROM-exporter
and firmware research tools; neighboring writable checkouts are unnecessary.
The helper restores the monitor interface, NetworkManager management and
original link state before analysis. On timeout or interruption it preserves
partial captures and reports failure. Client exit signals the helper to clean up.

First radio observations and Block ACKs do not prove decoder acceptance.
Missing sniffer packets do not prove GamePad loss. Format age uses nearby AP
beacons as an estimate of the GamePad's synchronized clock. Compare reports
alongside visible corruption and service resync counters before changing code.

## Install once

Dependencies are Python 3 with `cryptography`, Qt 6 Core/DBus, Polkit,
NetworkManager, `iw`, `ip` and `tcpdump`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
pkexec /usr/bin/cmake --install "$PWD/build" --component CaptureWorkflow
```

The component installs only the capture client/helper, analysis modules and
Polkit policy. It does not replace or restart the running Barista engine/service.
The dedicated `org.barista.capture-session` action defaults to an administrator
prompt with temporary authorization retention. The helper accepts only two
wireless interface names and a bounded duration; it accepts no command, output
path, credential path or executable from the caller.

For a developer workstation, explicitly opt in your local desktop account:

```sh
cmake -S . -B build -DBARISTA_CAPTURE_AUTH_USER=isaiah
pkexec /usr/bin/cmake --install "$PWD/build" --component CaptureAuthorization
```

This installs `/etc/polkit-1/rules.d/49-barista-capture.rules`, allowing that
**local, active** account to use the capture action and the existing
`org.barista.manage-session` action without a prompt. That existing action covers
start/stop, pairing and system preparation; the capture client uses start/stop.
This is an explicit developer-machine
setting, not a package default. Remove that rule to restore normal prompting;
clearing the CMake option does not remove an already-installed rule.

Codex can run the same `barista-capture` command from the active desktop session
once installed; sandbox permission to access the system bus remains separate
from Polkit authorization. No GUI or Cemu process runs as root.
