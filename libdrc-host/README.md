# libdrc-host

Core reusable library for Wii U GamePad host behavior.

Current ownership:

- `MacAddress` parse/format utilities.
- Pairing code/symbol conversion:
  - `♠=0`, `♥=1`, `♦=2`, `♣=3`
  - pin-byte encode/decode
  - pairing SSID builder (`WiiU<mac_minus_last_nibble><mac>_STA1`)
- Session state machine (`Idle -> Pairing -> Runtime`) with transition guards.

Roadmap:

- Hostapd event parsing and semantic events.
- Rich error taxonomy and diagnostics model.
