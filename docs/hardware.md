# Wi-Fi compatibility

[Back to README](../README.md)

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

Intel adapters (`iwlwifi`) work, but their driver **self-manages its regulatory
domain**, which needs one extra thing from the environment: the firmware adopts
a country only after hearing it from a neighboring 5 GHz access point during a
scan, and falls back to the restrictive `00` world domain when nothing keeps
supplying one. Barista scans to recover this automatically, so no manual setup
is required, but a machine in true radio isolation has nothing to learn a
country from and cannot host the pairing AP. `iw reg set` and
`DRCD_REGULATORY_COUNTRY` cannot substitute — a self-managed domain ignores
both. Tested with the **AX200** (desktop M.2, no ACPI regulatory tables, the
harder case; laptops seed the country from firmware). Two `runtime-qos` tuning
calls report `Operation not supported` on this driver and are skipped safely.

For additional confirmed and incompatible hardware reports, see
[Vanilla Wii U's Wireless Compatibility wiki](https://github.com/vanilla-wiiu/vanilla/wiki/Wireless-Compatibility).

For setup failures, see [troubleshooting](troubleshooting.md).
