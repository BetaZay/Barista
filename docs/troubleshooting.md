# Troubleshooting and support logs

[Back to README](../README.md) · [Wi-Fi compatibility](hardware.md)

## Collect a support report

Open **Settings → Support** when a session does not start, pair, connect, or stream
correctly. Barista shows a diagnostic code with a suggested next step. Use
**View log** to inspect an individual run, pairing-cycle, or maintenance log, **Open log
folder** to browse retained logs, or **Copy support report** to collect the
current system and session summary for a bug report.

Support logs are designed to be safe to share. Each pairing or connection run
contains coded state changes plus sanitized detailed engine and hostapd output,
including adapter checks, channel selection, AP setup, association, and cleanup.
MAC and IP addresses, SSIDs, pairing codes, credentials, usernames, key material,
and packet dumps are replaced with redaction markers. Detailed unsanitized engine
logs remain available to administrators for local investigation and should be
reviewed before being shared.

## Pairing search

If the pairing AP becomes ready but the GamePad returns to its pairing screen
without authenticating, check that the installed bundled hostapd includes the
Nintendo WPS discovery patch (`0015`). The original Vanilla-fork integration
retained Nintendo encryption but dropped the Nintendo WPS advertisements.
A local 2026-10-23 pairing log on `rtw89_8852be` showed standard WPS Version2
advertisements and no received authentication, while an earlier connection
with saved credentials succeeded. The restored format has deterministic
encoding/parsing coverage; a new physical pairing remains the confirmation.
Updating only the engine leaves an old hostapd in place.

Wait for **Pair now** before pressing SYNC and entering the displayed symbols.
Barista searches each channel in its pairing plan once, normally waiting about
20 seconds per channel. A full nine-channel search can take more than three
minutes, including network setup. Authentication activity locks the search to
that channel and leaves time for pairing to complete. Cancel stops the search.
Probe requests alone do not identify the GamePad: Barista logs them without
replacing the pairing PIN or narrowing authorization to their source address.
Switching back to the saved GamePad's runtime network after a timeout does not
mean pairing succeeded; the GUI waits for a GamePad connection before displaying
its success message.

## Regulatory restrictions

If Barista reports `AP_REGULATORY_BLOCKED`, check the active country with
`iw reg get`. A `country 00` world domain commonly marks 5 GHz channels
`no IR`, so the kernel prevents hostapd from transmitting AP beacons; firewall
changes do not affect this failure. The GUI suggests a two-letter country from
the desktop locale and asks the user to confirm that it matches the machine's
physical location. When confirmed, Barista can temporarily replace `00` for
the session. It restores the previous domain at shutdown unless another
component changed the setting in the meantime.

If `iw reg get` marks the phy itself `(self-managed)` — Intel `iwlwifi` does —
then the country override above cannot apply, because such a driver ignores the
kernel regulatory core. Barista attempts recovery with a scan, but a usable
channel before AP startup does not guarantee that the firmware will permit it
after switching to AP mode. An AX201 report shows all nine pairing channels
remaining blocked at that stage. There is
no module option to disable this behavior; `iwlwifi.lar_disable` was removed
from the kernel years ago and is silently ignored.

## Radio blocks and firewalls

Two host settings produce failures that look like Barista faults:

- A radio soft-block reports `AP_START_FAILED` even though the adapter passes
  every capability check. Check `rfkill list` and clear it with
  `rfkill unblock wifi`.
- A host firewall that denies inbound traffic lets the GamePad associate and
  complete its key handshake, then stalls with protocol command timeouts and no
  `dhcp: sent OFFER` line, because the GamePad's DHCP and protocol replies are
  dropped locally. Allow inbound UDP on the GamePad interface for port 67 and
  the runtime ports 50010 and 50020-50025.

Shareable logs are stored in `/var/log/barista/support`. Detailed private logs
are stored in `/var/log/barista/private` and remain root-only.
