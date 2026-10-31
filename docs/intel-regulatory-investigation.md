# Intel regulatory startup investigation

Investigation date: 2026-10-31. Hardware validation of this restoration is pending.

## Evidence and regression

[Issue #1](https://github.com/BetaZay/Barista/issues/1) originally reported
AX200 pairing failures. Setting the global country did not help. After
[thefriendlyhedgehog's PR #3](https://github.com/BetaZay/Barista/pull/3) was
merged as `97c719d`, the reporter confirmed pairing worked. Their latest
[comment](https://github.com/BetaZay/Barista/issues/1#issuecomment-6548271837)
attributes a new failure to Intel firmware, but supplies no new kernel version,
loaded firmware version, Barista version, regulatory dump, or startup log.
It does not establish which component changed.

PR #3 tested an AX200 with kernel 7.2.4 and firmware `77.aa2dd297.0`
(`cc-a0-77.ucode`). It supplied two recovery stages:

1. Scan in the engine when capability checks find all pairing channels blocked.
2. Scan inside hostapd after channel resolution or hardware-mode selection
   fails, then re-read channel capabilities and retry once. This covers a
   change that occurs after the engine's preflight checks.

The second stage was removed from the active patch list by `3440976`, the
switch to Vanilla hostapd. The pinned Vanilla revision
`a2f705daa143791180754100194c91714f7e8801` has no equivalent helper. The engine
recovery remains, and patch `0014` remains on disk, but neither substitutes
for applying that patch to hostapd. This is a confirmed source regression;
its relationship to the latest issue report is not yet confirmed.

The `AP timing replacement` stash contains no hostapd build changes. Both its
configuration and the working tree before this restoration select the same
Vanilla revision with patches `0007` and `0015`.

## Restoration

Port and activate `0014` on the pinned Vanilla source. Keep the original
bounded six-second wait after an accepted passive scan and retry channel
selection once. Do not retry or wait when the driver rejects the scan.
Successful channel selection does not scan or wait. The helper is scoped to
`CONFIG_TENDONIN`; it never clears NO-IR or other channel restrictions.

Barista must also allow the retry to finish. Its startup reader previously
treated an intermediate `not allowed for AP mode ... NO-IR` line as an immediate
failure. Preserve that diagnostic while waiting for `AP-ENABLED`, a terminal
`AP-DISABLED`/`AP-NO-IR` event, process exit, or the existing startup deadline.
A scan completion message alone does not mean the AP is ready.

Deterministic coverage uses the actual patched scan helper with mocked scan
and sleep functions, and tests warning/recovery/terminal-event sequences in
the startup reader. No test uses a real adapter. The six-second wait still
blocks hostapd's event loop, as the earlier workaround did; making that wait
event-driven would be a separate change with its own lifecycle tests.

Both the engine and bundled hostapd must be rebuilt and installed to exercise
the complete restoration. Replacing only the engine leaves the missing
AP-mode recovery unchanged. Rollback is to rebuild the previous engine and
hostapd from the same revision with these changes reverted. No system binary,
driver, firmware, radio state, or privilege rule was changed in this investigation.

## Driver and firmware limits

The [upstream iwlwifi documentation](https://wireless.docs.kernel.org/en/latest/en/users/drivers/iwlwifi.html#features)
documents AP support on 2.4 GHz and explicitly excludes 5 GHz AP support due to
LAR. The earlier AX200 success is evidence for that tested combination, not
a general Intel support guarantee.

The [removal of `lar_disable`](https://github.com/torvalds/linux/commit/f06021a18fcf8d8a1e79c5e0a8ec4eb2b038e153)
explains that disabling LAR can violate driver/firmware expectations. Adding
the old module parameter back is therefore not an established solution for
current firmware. Likewise, changing wireless-regdb or the global country
does not force a self-managed PHY to allow an AP.

The [driver's MCC path](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/mvm/nvm.c)
sends a country/source request to firmware, receives channel information, and
applies the resulting regulatory domain. It also handles BIOS and firmware
MCC notifications. PR #3's nearby-AP scan explanation describes its observed
workaround; it should not be read as proof that Country IEs are the only input
on every platform or that a specific new firmware update caused this report.

There is currently no verified firmware patch for the reported regression.
First restore the missing application workaround and identify the failing
boundary. Clearing host-side flags would not demonstrate that firmware can
start the AP, and should not be described as a firmware fix.

## Evidence needed for an affected AX200

Collect the following from the affected machine, keeping credentials out of
shared logs:

- Barista build and bundled hostapd provenance; startup logs should show
  `regulatory-prescan:` if channel-selection failure reaches the restored helper.
- Kernel version, PCI device/subsystem ID, driver version, loaded firmware
  version from the kernel log, and installed firmware package version. The
  `-77` API filename alone does not identify the firmware contents.
- `iw reg get` and `iw phy <selected-phy> info` before takeover and during the
  failure, distinguishing the global country from the selected self-managed PHY.
- The complete support/startup log showing whether the scan was accepted,
  whether usable channels returned, and whether `AP-ENABLED` followed.

If the restored workaround still fails, compare a known working and failing
kernel/firmware combination on the same machine and actual country, changing
one component at a time and recording loaded versions. A firmware package
change alone is not sufficient evidence unless the loaded firmware changed.
That comparison determines whether to pursue hostapd lifecycle handling, a
driver regression, or a firmware report with Intel. Physical pairing and
streaming remain required to establish compatibility.

## Local verification

- `cmake --build build --parallel 4`: passed with the existing Debug cache.
  The rebuilt hostapd contains all three `regulatory-prescan:` log markers.
- `ctest --test-dir build --output-on-failure`: **63/63 passed**, including
  the new mock-driver recovery test, startup-output tests, and GUI smoke tests.
  Run with host access for local socket fixtures; elapsed time 112.32 seconds.
- `git diff --check`: passed.
- Installation and affected Intel hardware pairing/streaming: not performed.
