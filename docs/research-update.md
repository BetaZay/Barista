# Firmware/encoder research applied, 2026-09-05

This update applies measured discrepancies, not a claim that all physical
recovery causes are solved. Research and original evidence remain under
`../../wiiu-code/research`.

## Chroma contract

The default x264 psy settings lowered requested chroma offset 0 to effective -2
during initialization. Our headerless DRH reconstruction assumes PPS offset 0.
A strict syntax decode did not catch the resulting color/reference mismatch.
DRH parameter normalization now enforces zero after psy adjustment, leaving the
psy search itself enabled. Non-DRH encoders retain upstream behavior. The C++
wrapper checks the effective setting so a stale/incompatible library fails open
validation instead of silently emitting a mismatched stream.

`drcd_encoder_reconstruction_tests` runs slow and fast search, compares the test
probe's bytes with the actual production offline worker, and compares every YUV
pixel decoded with the reference PPS against x264's internal reconstructed
pictures. This guards more than syntax correctness. Requires Python and FFmpeg;
CMake registers it when both are present. No NumPy or firmware is required.

The physical pad's implicit register configuration has not been independently
measured. The correction matches our receiver contract and fixes the demonstrated
offline mismatch; it is not proof of the original hardware recovery cause.

## Format/video timing

The firmware's sync task discards updates with unsigned counter-minus-timestamp
>=8000. Real-console format packets precede same-timestamp video by median 5134us
and have median AP-clock age 1150us. Our old live sender put them back-to-back,
with format age 6363us in the independent Mac capture; 124/5461 observations
crossed 8000us. The new lead is 5000us with format timestamp age 1250us.

An independent 59.94Hz scheduler publishes formats even when the encoder has no
video ready. After encoding, the producer reserves a future eligible format slot
and submits video to the bounded serial sender. The sender waits for that slot's
video deadline and preserves chunk/packet offsets and order. There is no unbounded
pending-frame queue or discard of encoded reference pictures. Format and video
carry one timestamp, never independently re-stamped after the format is sent.
PCM remains independent. The clock can publish the next format while the serial
sender is still finishing the preceding video's last chunks.

Late encoding leaves format-only slots. A delayed format publisher rebases its
next deadline without a catch-up burst of stale messages. An actual video
start >=1ms late produces rate-limited diagnostics. These are sender lateness
measurements, not readings of the GamePad's LCD phase or reset reason. Severe OS
stalls can still miss deadlines; this update does not claim hard realtime behavior.

## Console recovery slots and coalescing

All six captured console IDRs have a format-only slot at +16683/16684us and P
video at +33366/33367us, with consecutive video packet sequences across the gap.
The live scheduler now leaves one video slot empty after every IDR, including
startup. Formats continue. `DRCD_IDR_PAUSE` no longer controls behavior; all-IDR
diagnostics intentionally have a maximum video rate of approximately 30fps.

Ordinary GamePad requests enter a host recovery lifecycle at IDR selection.
Repeated requests are coalesced until the first resumed P finishes host delivery;
the pending request latch is drained before reopening the gate. Generation IDs
prevent an older P completing a newer recovery being encoded concurrently.
Source changes can still explicitly request an IDR. This models the observed
sequence and firmware's state-guarded/coalesced recovery, not its unknown exact
hardware-event timing. We do not apply the console wireless-failure-only discard
gate to every generic GamePad request. Raw replay retains its recorded schedule.

Unit tests exercise idle/slow-producer formats, recovery slots, legacy timestamp
selection, publishing stalls and errors. Loopback tests inject requests every
2ms through the IDR/gap, verify P resumption and format-only slots, and retain
packet-order, audio, input and timestamp checks.

## What was deliberately preserved

Actual AP TSF clock, QP32 default, IDR/init policy, finalized CABAC boundaries,
six-row chunks, packet spreading, PCM sizing/cadence/timestamps, input and command
keepalive behavior. No firmware flashing, register writes, pairing changes, new
runtime dependency or additional default environment switch.

The earlier replay already sent format packets early and still received recovery
requests. Also, generic recovery messages discard internal reasons and repeat in
the waiting state. These findings prevent attributing every request to late format
delivery, invalid H.264 or a dropped packet. The recovery lifecycle above is a
bounded compatibility correction; initial reset causes remain unproven. No
keepalive rewrite is included.

## Verification and physical test

```sh
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python scripts/test-encoder-contract.py
sudo bash scripts/test-media-profile.sh baseline cemu /tmp/drcd-recovery-slots.pcap
```

Unit/integration tests need localhost UDP and Unix sockets; a sandbox denying
socket creation cannot run them. The physical capture is a separate manual
acceptance test, not performed by CTest. Inspect sustained motion/color, audio
continuity and latency, as well as capture completeness and timing. A lower
generic-request count alone is insufficient to establish correct playback.
