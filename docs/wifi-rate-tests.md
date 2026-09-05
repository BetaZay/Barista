# Post-association rate tests

These are opt-in hardware tests, not new streaming defaults. The earlier bitrate
clear test worsened playback and is not included. The startup "fixed MCS 6" log
was not proof of the actual rate: independent Mac captures reported MCS 7 too.
The test launcher applies the same requested HT mask only after the GamePad is
authorized, then verifies the result using station samples and a Mac capture.

## Procedure

Stop the previous drcd/capture. Keep Cemu using its existing drcd media bridge.
For each test, start a fresh Mac sniffer capture before starting the PC command
and connecting the GamePad. Use the runtime channel (recent runs used channel
36; check `iw dev wlan0 info` when the AP is running). Do not reuse capture names.

From `drc-project`, run one at a time:

```sh
sudo python scripts/test-wifi-rate.py 6 /tmp/drcd-rate-mcs6.pcap
sudo python scripts/test-wifi-rate.py 5 /tmp/drcd-rate-mcs5.pcap
sudo python scripts/test-wifi-rate.py 4 /tmp/drcd-rate-mcs4.pcap
```

Save their Mac captures as `mac-rate-mcs6.pcap`, `mac-rate-mcs5.pcap`, and
`mac-rate-mcs4.pcap`, respectively. Do not paste all three PC commands as a batch:
each needs a fresh Mac capture/connection and the same scene repeated. The
launcher rejects an existing drcd process or preexisting output companions.

Each test waits up to 120 seconds for authorization on the 5 GHz AP AND current-session
DRC readiness, a successfully sent first video frame, and the pad's active-stream
flag. It then records 20 seconds with the existing startup settings, applies its requested mask, then
records 30 seconds and stops through the normal capture cleanup. There is no
automatic-rate test phase. Before the change, ordinary startup retries are allowed:
lost readiness restarts the baseline after a fresh active session, within the
startup wait budget. Old readiness markers are invalidated on session/AP reset.
These markers improve the startup gate but do not prove flawless visible playback.
Ctrl-C ends early; after the rate change, an observed disconnect ends the test.
A rate-command failure or capture failure also ends the test. If cleanup is reported pending,
wait for it to finish before starting another run. The live view can disconnect
when the test ends because the launcher deliberately shuts down the AP.

Use a repeatable motion scene; this is not a pixel-identical source benchmark.
Note when artifacts or the instability warning occur. A warning without actual
disassociation does not automatically abort the test; press Ctrl-C if needed.

`--baseline-seconds` and `--seconds` accept 5–120 seconds. `--dry-run` prints the
complete plan without sudo, processes, output files or Wi-Fi changes.

## What changes, and what does not

The only applied test command is:

```sh
iw dev wlan0 set bitrates ht-mcs-5 6 lgi-5
```

Substitute 5 or 4 for the selected test. It requests an HT MCS mask and long
guard interval. Legacy rates remain allowed; it is not a guarantee that every
on-air packet has this MCS. No TID retry limits, queue flushes, aggregation
changes, firmware writes or channel changes are introduced. Actual AP TSF,
format/recovery scheduling, PCM, QP32, encoding and existing RTS behavior remain
under the unchanged baseline launcher.

Stopping uses the existing capture cleanup to restore its rate/RTS policy and
stop the AP. It does not claim to reconstruct an arbitrary preexisting driver's
rate-control state. Starting each next test creates a fresh runtime session.

## Evidence

Each run produces the usual PC pcap, IP pcap, log and first-IDR artifacts, plus
`*.rate.jsonl` with streaming readiness, baseline/restart/rate phase boundaries, exact command,
exit status, and station statistics sampled about every two seconds. Markers
contain PC wall/monotonic times. PC and Mac wall clocks may differ; do not align
them blindly. Readable station statistics are supporting evidence, not proof
of every packet's rate or successful GamePad decoding.

The Mac rate summary needs neither root nor pairing credentials:

```sh
python scripts/summarize-wifi-rates.py ../mac-rate-mcs6.pcap
```

It reports HT MCS/guard interval, legacy rates and retry observations in five-second
bins relative to the first observed AP-to-pad data frame. Unknown fields and
truncated tails are reported. VHT/HE rates are not decoded by this focused helper.
Retry observations are not a packet-loss percentage. Authenticated delivery/age
analysis remains available through `../drc-eeprom-exporter/analyze_delivery.py`.

Local verification (no hardware):

```sh
python scripts/test-wifi-rate-tests.py
python scripts/summarize-wifi-rates-tests.py
python scripts/test-media-profile-tests.py
```
