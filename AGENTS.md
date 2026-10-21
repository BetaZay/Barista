# Repository Guidelines

## Project Structure & Module Organization

Barista is a C++20/CMake project that connects a Wii U GamePad to a desktop application. `app/` contains executable entry points, the Qt 6 GUI, D-Bus service, and branding. `core/api/` holds the portable application and media interfaces, while `core/drh/` contains the GamePad protocol, server, encoder, and Linux radio backend. Tests for portable code live together in `core/tests/`. Linux installation integration remains in `packaging/`. Keep changes to bundled upstream code in `third_party/` deliberate and isolated. Utility and hardware-analysis scripts live in `scripts/`.

## Build, Test, and Development Commands

Use the documented Ninja build flow (see `COMPILING.md`):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For UI or portable-core work, avoid Linux radio dependencies with `-DBARISTA_BUILD_ENGINE=OFF` and a separate build directory such as `build-desktop`. Build release packages with a Release configuration and run `cpack --config build/CPackConfig.cmake -G DEB -B dist` (or `RPM`).

## Coding Style & Naming Conventions

Write C++20 with four-space indentation, braces on their own lines, and no required compiler extensions. Follow nearby code: `PascalCase` for types and public methods, `camelCase` for local variables and parameters, and `m_`-prefixed instance members. Name implementation and test files after the unit they cover, e.g. `single_instance.cpp` and `single_instance_test.cpp`. No repository-wide formatter or linter is configured; preserve the style of the edited file and avoid unrelated reformatting.

## Testing Guidelines

CTest drives the C++ test executables. Add focused tests alongside the relevant module, register them with `add_test`, and use descriptive names such as `drcd_packet_schedule_tests`. Run the full CTest suite after changes; for GUI work, include the existing smoke tests. Hardware/radio scripts are supplementary and must not replace deterministic unit coverage.

## Commit & Pull Request Guidelines

Recent history uses short imperative subjects, commonly conventional prefixes: `feat:`, `fix:`, `refactor:`, `docs:`, `packaging:`, and `ui:`. Keep commits narrowly scoped. Pull requests should explain the behavioral change, list verification commands, link related issues when present, and include screenshots for GUI changes. Call out hardware, privilege, service, or Wi-Fi-adapter testing explicitly.

## Security & Configuration

The GUI runs unprivileged while the installed service owns privileged radio work. Never add a workflow that launches the GUI via `sudo` or `pkexec`; keep AppHook socket permissions scoped to the desktop user.

## GamePad Video Experiment Workflow

Read `docs/video-experiment-checklist.md` before selecting a change. Keep the
experiment history there and detailed evidence in
`docs/gamepad-recovery-experiment.md`. Mark completed ideas accepted, rejected,
inconclusive, or abandoned; do not repeat a rejected setting without a new
hypothesis or the user's request. Fast search preset 9 was visually worse and
was reverted. The packet replay workflow was abandoned by the user.
The 250-ms requested-IDR cooldown was also rejected after the user reported
more lag and corruption; recovery requests must not be delayed by that gate.
The resumed-P-start recovery trial is provisionally retained after a tentative
visual improvement: settling requests clear before that P starts, so requests
during it remain eligible. Preserve its generation check and injection test.
The native-only CAVLC residual bypass is provisionally retained after the user
reported slightly less corruption. Preserve CABAC/reconstruction state and
full CAVLC for diagnostic callers; verify both paths when changing it.
Proactive IDRs after 60 encoded frames without one are retained because the
user reports corruption clears instead of lingering. Preserve immediate
requested recovery and the format-only slot. This is a mitigation, not a fix
for the initiating corruption; record its extra bandwidth, slots and requests.
The mock-radio periodic-IDR test verifies unsolicited resets after startup.
Native integer-motion search is provisionally retained after a tentative visual
improvement. Preserve preset 5 and its other behavior; only fractional search
is disabled by the explicit create flag. Diagnostic reconstruction comparisons
must select the same policy, with full CAVLC preserved. Corruption remains;
do not present this compatibility hypothesis as a proven decoder defect.
The native no-P-skip policy is provisionally retained after another tentative
improvement; the same transition spots still corrupt. Preserve explicit inter
coding and residual analysis, and align diagnostic comparison parameters while
keeping full CAVLC. Its create flag disables both early approximate skip and
all-zero inter-to-skip conversion; ordinary callers retain skip by default.
Zero-motion P prediction is now the retained physical comparison baseline:
the user reports essentially zero observed corruption in that run. Keep explicit residuals,
periodic IDRs and the Release build. Save the working engine before future
changes; `/tmp/barista-engine-zero-motion-retained-20261004` is the tested
2026-10-04 copy (hash recorded in the report). Motion vector coding, predictors
and interpolation remain separate hypotheses; do not claim a root cause from
this test alone. Recovery request counts stayed similar despite the visual
improvement and must not replace the user's playback comparison.

Use one focused behavior change per hardware trial. Preserve the smoother
baseline: 1694-byte video payloads, kernel receive timestamps for the hardware
TSF anchor, native encoder preset 5, fixed QP 32, and restricted planar
prediction. Record intentional deviations and provide a rollback. Keep
unrelated uncommitted work intact.

Build and run the full CTest suite before installing an experiment. The local
socket tests need host access outside the restricted sandbox. Unit fixtures
must mock radio/interface state and never depend on attached hardware. Record
test failures and fixes accurately; do not report a failed suite as passing.
Avoid CPU-heavy tests and benchmarks during hardware recording.

Use `build-gamepad-release/` with `CMAKE_BUILD_TYPE=Release` for hardware video
trials and run its full CTest suite. The Debug engine caused substantial encode
overhead; Release improved physical framerate, while corruption/smearing
remained. Preserve `build/` for Debug development, but do not install that
engine over the retained Release baseline. Verify the cache build type.

The normal runner builds development binaries but does **not** install the
engine. The service executes `/usr/local/libexec/barista/barista-engine` on
this workstation. Back up that binary to a new path in `/tmp`, install the
tested `build-gamepad-release/app/drcd` with Polkit, and compare their SHA-256 hashes. Verify
the selected setting in the live support log; shell environment variables are
not inherited by the service's explicitly constructed engine environment.
Confirm installation paths on other machines instead of assuming this layout.

Use the headless live workflow:

```sh
python3 scripts/run-gamepad-experiment.py \
  --build-dir build-gamepad-release \
  --game '/mnt/Data/Emulation/roms/wiiu/roms/The Legend of Zelda - The Wind Waker HD (USA).wua' \
  --seconds 90
```

The desktop user runs this command. The installed capture helper owns bounded
privileged radio work through `org.barista.capture-session`; the existing
`org.barista.manage-session` action manages the session. This user's approved
local-active-account rule permits those actions without repeated prompts.
System installation and sandbox escalation remain separate. Do not broaden
the privilege rules for an experiment or launch desktop applications as root.

Use wlan0 for the paired AP and wlan1 for capture on different PHYs. Keep the
GamePad off until `READY`, ring the terminal bell at that prompt, and tell the
user when to turn it on. The runner launches
`/home/isaiah/Downloads/Cemu-2.8-x86_64.AppImage` only after confirmed connection;
it opens no Barista frontend by default. Check for an existing Cemu process
before starting; close the test instance gracefully or reuse it deliberately,
without killing unrelated applications.

Retain the `RESULT` capture directory, experiment logs, input/settings and
installed-engine hash. Authenticate radio decryption, compare host/radio
frames, format age, chunk timing, Block ACK evidence and encoder timing.
The latest real-console reference is `../wiiu-real-fresh-165.pcap`; recent
`/var/lib/barista-captures/` runs are Barista sessions. Vanilla controls are
in this repository's `local-captures/`. Read neighboring firmware research
when needed and keep credentials/EEPROM keys out of logs and documentation.

Ask for the user's visual comparison during a trial and record their answer.
Recovery requests can repeat and their count is not a count of visibly
corrupt frames. Block ACK proves MAC receipt, not decoder acceptance; missing
sniffer frames are observation gaps, not proven receiver loss. FFmpeg decode
success does not establish physical GamePad compatibility. Separate measured
facts from hypotheses and differences in interactive game scenes.

If the user reports worse video, reject the change, save the bounded capture,
and revert both source and installed engine. Verify the restored binary and
tests. If an interruption leaves a monitor behind, first establish that no
capture process owns it, then restore only the experiment's monitor and
NetworkManager state. Keep cleanup and session ownership scoped to the test.
