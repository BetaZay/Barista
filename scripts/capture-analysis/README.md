# Local capture analysis

These modules are carried into the root-owned installation so capture analysis
never imports code from a writable checkout or depends on neighboring projects.
They were copied from the local `../drc-eeprom-exporter` and
`../wiiu-code/research` tools on 2026-10-04. `analyze-media-capture.py` and
`summarize-wifi-rates.py` originate in this repository's `scripts/`.

Adaptations: local import path in `analyze_clock.py`; a PMK entry point in
`decrypt_capture.py`; standalone distribution/wrap helpers and removal of
research artifact output in `analyze_format_clock.py`. Protocol parsing,
handshake/CCMP authentication, delivery matching, Block ACK interpretation and
format freshness calculations retain the original behavior. Preserve the
original synthetic fixtures when updating these modules.
