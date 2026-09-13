# Troubleshooting and support logs

[Back to README](../README.md) · [Wi-Fi compatibility](hardware.md)

## Collect a support report

Open **Settings → Support** when a session does not start, pair, connect, or stream
correctly. Barista shows a diagnostic code with a suggested next step. Use
**View log** to inspect an individual run, pairing-cycle, or maintenance log, **Open log
folder** to browse retained logs, or **Copy support report** to collect the
current system and session summary for a bug report.

Support logs are designed to be safe to share. They contain coded state changes,
versions, adapter driver information, and health checks. They exclude MAC and IP
addresses, SSIDs, pairing codes, credentials, usernames, and raw hostapd output.
Detailed private engine logs remain available to administrators for local
investigation and should be reviewed before being shared.

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
kernel regulatory core. Barista instead scans to make the firmware re-adopt a
country, which requires a neighboring 5 GHz access point within range. There is
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
