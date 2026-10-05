# GamePad video recovery experiments, 2026-10-04

The retained Release baseline now uses zero-motion P prediction with explicit
residual coding. In the latest physical trial, the user reports essentially
zero observed corruption. All 45 tests in the current development workspace
pass; observed host cadence is 58.02 fps with no logged encoding overruns.
The working engine is saved with its hash in the zero-motion section below.
This is a demonstrated playback improvement in that session, while the exact
fault in the motion-compensated path remains unresolved.

The following sections preserve the evidence and outcomes of earlier trials.

The latest captures authenticate and reconstruct valid software-decodable
video, but the physical GamePad continues requesting recovery. A complete,
Block-ACKed frame is evidence of MAC receipt, not decoder acceptance. A request
shortly afterward does not identify which frame or internal firmware condition
caused it. The firmware also repeats requests while waiting for recovery.

## Evidence before the change

Baseline: `/var/lib/barista-captures/20261004-154447-fd67mngi`.
The radio handshake authenticates with the Nintendo-rotated PTK. All 58,122
pairwise encrypted observations examined authenticate. The host stream has
5,153 complete frames and one partial boundary frame. Reconstructing the host
stream with `scripts/reconstruct-media-capture.py` yields 53 IDRs and 5,100 P
frames; FFmpeg decodes them without warnings.

The independent radio analyzer sees 49 complete, fully Block-ACKed IDRs;
33 are followed by a recovery request within 5–50 ms after full ACK. Format
freshness is healthy at first radio observation: median 1,776 us, p99 2,832 us,
with one of 5,289 formats at least 8,000 us old. Format-to-video lead is 5,076 us.
These measurements do not support treating late formats as the dominant cause.

The preserved real-console reference was reauthenticated locally from
`../wiiu-real-fresh-165.pcap` using the existing EEPROM dump. All six reference
IDR episodes resume P video at a timestamp step of exactly 33,367 us. Baseline
Barista also emits an intervening format-only slot and resumes with P, but its
step ranges from 32,501 to 33,827 us (median 33,042 us).

## Change and verification

The radiotap clock previously anchored each hardware TSF to the time userspace
drained that packet. Socket queue delay consequently changed the extrapolated
clock anchor. The new path enables `SO_TIMESTAMPNS`, reads `SCM_TIMESTAMPNS`
with `recvmsg`, converts kernel receipt time into the steady-clock domain, and
uses that time for extrapolation. Hardware TSF remains the clock source.
Missing/future/stale/truncated and out-of-order samples are ignored. The
register-based AP clock path is unchanged.

New deterministic tests verify that delays of 0, 500, 2,500 and 20,000 us do not
move the sample anchor, and invalid timestamps are rejected. All 42 CTest
entries pass outside the sandbox, which permits their required local sockets.
The installed engine matches the tested build, and its live log confirms that
kernel receive timestamping is enabled.

The unprivileged experiment runner now emits BEL at `READY`, also writing to
an ancestor's interactive terminal when available. The default run starts no
Barista frontend. Cemu 2.8 is launched only after connection is confirmed.

## Physical result

Changed-engine capture: `/var/lib/barista-captures/20261004-155328-a1vbpa18`.
Both samples recorded for 90 seconds following GamePad traffic, launched the
same Cemu 2.8 AppImage and Wind Waker title, and authenticated successfully.
They were separate interactive runs, not identical rendered-frame replays.

| Measurement | Baseline | Kernel receive timestamp |
| --- | ---: | ---: |
| Unique over-air recovery requests | 168 | 300 |
| Complete observed radio video frames | 4,827 | 4,824 |
| Fully Block-ACKed video frames | 4,796 | 4,805 |
| Fully Block-ACKed IDRs | 49 | 90 |
| Fully ACKed IDRs followed by requests within 5–50 ms | 33 | 55 |
| Median format age | 1,776 us | 1,793 us |
| p99 format age | 2,832 us | 2,674 us |
| Formats at least 8,000 us old | 1 | 1 |
| Median format-to-video lead | 5,076 us | 5,061 us |
| Median host IDR-to-next-P timestamp step | 33,042 us | 33,088 us |

The changed host stream reconstructs 95 IDRs and 4,977 P frames, again with
zero FFmpeg warnings and one incomplete capture-boundary frame. No adjacent
IDR pairs occur. There is one additional format-only slot in an exceptional
slow-encode episode, giving a maximum IDR-to-P timestamp step of 49,987 us.

This is a clock-anchor correctness change, **not a demonstrated corruption
fix**. The user reports that the changed-engine run looked **much smoother**.
This is useful physical presentation evidence, and the correction is retained.
The generic request counter can repeat while the receiver waits for recovery;
it is not a direct count of visible corrupt frames. The hardware trial did not
reduce recovery requests, and the clock-age medians
are nearly unchanged. Queue delay was not established as the dominant timing
error. Different game interactions prevent assigning the increase in requests
to this change alone. Decoder assumptions, per-chunk consumption and display
restart state remain open; changing another timing constant without isolating
one of these conditions would not settle the cause.

Detailed timing, delivery, PHY/drop counters and sanitized service reports are
retained beside both captures. Radio coverage gaps remain observation gaps,
not proven GamePad packet loss.

## Next trial: fast encoder search

The user abandoned the packet replay trial after it failed to work on the
physical connection. The next live trial uses MiniH264 search preset 9 instead
of preset 5. This changes search effort; fixed QP 32, planar-prediction
restriction, packet payload size, clock correction and packet pacing remain
the same. The experimental default was preset 9; after the user reported
**worse / more corruption**, that default was reverted to preset 5.
`DRCD_FAST_ENCODE=1` still explicitly selects preset 9 for diagnostics.

The motivation is measured live encoding delay. One interval in the smoother
run averaged 13,982 us per frame, with IDRs averaging 33,139 us and 17 frames
over the 16,683 us budget. Independent format publication continues while
encoding is busy, so these delays can create additional format-only slots and
reduce video cadence. Whether they explain visible corruption is unproven.
Faster search can also change compressed size and picture quality, so compare
encoder timing, radio delivery and visual observations together.

Fast-search capture: `/var/lib/barista-captures/20261004-161257-49gw41n8`;
runner logs: `/tmp/barista-experiment-20261004-161237-pk9nnj7q`.
The live log confirms preset 9. All 57,336 encrypted observations examined
authenticate. The host stream reconstructs 61 IDRs and 5,011 P frames with
one incomplete capture-boundary frame; FFmpeg produces zero warnings.

Across 18 five-second timing intervals, the earlier preset-5 run averages
9,176 us per encoded frame versus 6,570 us for preset 9. Over-budget counts
are 241 versus 146, but mean IDR time is 26,441 versus 28,241 us. These were
different interactive scenes, so the measurements do not isolate search
effort. The fast trial has 202 over-air recovery requests, 4,485 fully
Block-ACKed frames and 548 incomplete observed radio frames, compared with
300 requests, 4,805 fully ACKed and 249 incomplete observations in the smoother
run. Observation gaps are not proven receiver loss. In particular, fewer
requests did **not** mean a better physical picture: the user reports more
corruption. The fast default is rejected and preset 5 restored in source and
the installed engine. The hardware clock correction remains enabled.

The failed replay left an unused `baristasniff` monitor without a capture
process. It was removed and wlan1 returned to NetworkManager before this
trial. Its presence also exposed a lifecycle fixture reading real interface
existence; the fixture now mocks that state and passes with a live monitor
present. Capture and engine privileges were not expanded for this trial.

## Video option order trial

The next candidate changes only the order of video header options to match
the real-console and Vanilla control captures. IDR options become
`80 82 00 83 85 06 00 00`; P options become `82 00 83 85 06 00 00 00`.
The option values and encoded payload bytes remain the same for a given
encoded picture. Native preset 5 and the retained clock correction are used.
General option parsers accept both orders; possible firmware side effects
are the hypothesis being tested, not a confirmed cause of corruption.
Existing protocol self-tests already check both byte layouts and require
identical non-option header bytes and compressed payloads.

Capture: `/var/lib/barista-captures/20261004-162913-oovfvghi`;
runner logs: `/tmp/barista-experiment-20261004-162852-0_7wizov`.
Installed engine SHA-256:
`fb6233cf8407584a6a9e695bb63bcdcc22e02375e5cc1d102e5fe11323a5f5d9`.
All 43 CTest entries passed before installation. The live log confirms preset
5 and reference option order. Host packets independently confirm the exact
reference IDR/P option bytes; 223 IDRs and 4,371 P frames reconstruct with one
partial capture-boundary frame, and FFmpeg produces no warnings.

All 59,258 encrypted observations examined authenticate. There are 4,361
complete observed radio frames, 233 incomplete observations, 4,340 fully
Block-ACKed frames and 782 unique over-air recovery requests. Format age has
median 1,846 us, p99 3,329 us and maximum 7,754 us; no observed format is at
least 8,000 us old. Median format-to-video lead is 5,081 us. Recovery requests
rose compared with the smoother run, but interactive scenes differed and
their count does not establish a visible regression. The user reports **less
corruption than the preceding fast-search trial**, with remaining corruption
around **fade transitions and slide-ins**, and **slight jitter**. This is not
a confirmed improvement over the earlier smoother preset-5 run: this trial
also restores preset 5 relative to the immediately preceding fast trial.
Disposition is **inconclusive** for option order; the change remains installed
for continued testing, not as a demonstrated fix.

The log contains a five-second interval with 220 encoded frames, 70 over-budget
frames and 26 IDRs, followed later by an interval with 299 frames and no
over-budget frames or IDRs. Variable encoding/recovery load is consistent
with a possible jitter mechanism. Without timestamped visual markers it does
not establish which interval contained the reported transitions, or whether
encoding load causes corruption rather than following recovery requests.
The next investigation should target transition content, reconstructed
reference pixels and per-chunk delivery before selecting another change.

Rollback binary: `/tmp/barista-engine-before-option-order-20261004`.
Rollback in source: restore opt-in behavior for `DRCD_REFERENCE_VIDEO_OPTIONS`
in `MediaStreamer::video_loop`, build/test and reinstall. The updated
`AGENTS.md` records this workflow, and `docs/video-experiment-checklist.md`
records trial outcomes for future work.

## Fixed chunk deadline trial

The user reports corruption mainly around fades and sliding graphics, with
slight jitter. The next timing hypothesis is that delayed video starts also
delay all later chunk deadlines: the sender previously slept relative to
`frame_send_started`, even though the matching format already committed the
frame's timestamp and timeline. The trial instead sleeps relative to the
original `frame_deadline` for every packet. It retains packet order and all
encoded reference frames. If a deadline is already past, that packet is sent
immediately; measure compressed spacing as well as completion delay. This
does not remove initial wakeup delay or shorten encoding itself.

`scripts/analyze-video-deadlines.py` measures host video against the first
matching format observation plus the 5,000-us lead. In the preceding
option-order capture, all 4,594 complete matched host frames have first-packet
lateness median 64 us, p99 892 us and maximum 4,040 us. There are 35 starts at
least 1,000 us late. For the 171 frames with at least 15,000 compressed bytes,
last-chunk completion relative to the inferred video deadline has median
13,117 us, p99 14,164 us and maximum 15,045 us. No observed second-chunk end
exceeds 9,750 us; these observations do not establish decoder starvation.
Large compressed frames are not automatically identified visual transitions.

All 43 CTest entries passed. Installed engine SHA-256:
`bb21908b54d4726d595c6ffb3c739fd4eeaafedbd792592238469a341b13ee6a`.

Rollback binary: `/tmp/barista-engine-before-fixed-chunk-deadlines-20261004`.
Source rollback changes packet sleeps back to `frame_send_started + offset`
and removes the fixed-deadline status message. Preset 5, real-console option
order, payload size and kernel-clock correction remain the same.

Capture: `/var/lib/barista-captures/20261004-163540-0u5t24r3`;
runner logs: `/tmp/barista-experiment-20261004-163519-19q23062`.
The GamePad needed a connection retry before Cemu launched (PID 91310).
Recording started on pad radio traffic before the successful protocol
connection, so the nominal 90-second window includes startup and provides
only 48.09 seconds between the first and last observed host video packets.
The live log confirms fixed chunk
deadlines and preset 5. Radio decryption authenticates 31,880 observations
with one verified rotated PTK; 1,015 encrypted observations fail authentication
under available key coverage and are excluded. This is not a fully covered
radio reference across every connection attempt.

The host stream has 2,646 complete frames (75 IDRs, 2,571 P), plus one partial
capture-boundary frame. FFmpeg produces no warnings. The same host timing
analyzer gives:

| Host observation relative to format + 5,000 us | Previous option-order run | Fixed deadlines |
| --- | ---: | ---: |
| Complete matched frames | 4,594 | 2,646 |
| First-packet lateness p99 | 892 us | 94 us |
| First-packet lateness maximum | 4,040 us | 1,235 us |
| Last-chunk completion p99 | 13,124 us | 13,060 us |
| Last-chunk completion maximum | 15,045 us | 13,063 us |
| Large compressed frames (at least 15,000 bytes) | 171 | 33 |
| Large-frame last-chunk completion p99 | 14,164 us | 13,063 us |

One frame starts 1,235 us late but its second chunk ends at 3,064 us and final
chunk at 11,155 us, consistent with retaining later original deadlines rather
than adding first-packet lateness to every chunk. Only later packet deadlines
were changed; any difference in first-packet lateness itself cannot be
attributed to this mechanism. Both samples have no second-chunk end beyond
9,750 us. The shorter sample and different interactive content prevent a
controlled causal claim from these distributions.

Radio analysis reports 2,546 complete observations, 87 incomplete, 2,537 fully
Block-ACKed and 238 unique over-air recovery requests. Format age median is
1,787 us, p99 2,667 us and maximum 10,708 us, with one at least 8,000 us old.
Median format-to-video lead is 5,061 us. Raw request totals cannot be compared
as equal-duration active-video tests. The user reports **about the same
corruption, seemingly fewer lag spikes**. The change is retained provisionally
for pacing: this visual report is consistent with tighter later deadlines,
but the shortened trial limits confidence. It is not a demonstrated corruption
fix. Fade/slide-in corruption remains open, and another timing-only change
should not be assumed to solve it.

## Colored transition verification and recovery interval trial

The reconstruction test now includes 48 frames of colored textured fades and
a sliding panel crossing logical chunk rows. The normal preset-5 encoder's
production bytes match its reconstruction probe, and every FFmpeg-decoded
YUV pixel matches the encoder's internal reference. This catches reference
drift for these synthetic inputs; it does not establish physical GamePad
acceptance or cover every game transition. Existing motion and grayscale
fade checks remain in place.

The next live hypothesis concerns repeated requested IDRs during transitions.
Previous dense intervals contain as many as 26 IDRs in five seconds. Encoding
those full pictures is expensive, and the generic request does not establish
a distinct failure each time. Historical Vanilla recovery controls were
sensitive to restart policy, but do not establish an appropriate interval for
this native encoder.

The experiment uses a 250-ms minimum interval between **decisions** to start
requested recovery IDRs. A request outside the existing recovery lifecycle is
kept pending and serviced when eligible, even if no new request arrives.
Requests within IDR / format-only / first-P delivery remain coalesced as before.
Startup, source changes, encode failures and the explicit all-IDR diagnostic
mode retain their forced-IDR behavior. The interval is measured from the IDR
encode decision, not successful receipt, and does not imply an exact minimum
wire interval if encoding/scheduling delays vary. It can delay another valid
recovery request; reject it if that makes visible corruption worse.

New deterministic gate tests cover the interval boundary, pending requests,
coalescing, and source-change satisfaction. The live comparison retains
preset 5, option order, 1694-byte payloads, receive-clock correction and fixed
chunk deadlines. Rollback binary:
`/tmp/barista-engine-before-recovery-interval-20261004`.

All 44 CTest entries passed before installation, including the expanded
colored transition reconstruction and the new recovery gate tests.
Installed engine SHA-256:
`e0cb2be08689e560d9cf2bce50c4547b9e4ab673a3a5935df23045a71a4f9a71`.
Runner logs: `/tmp/barista-experiment-20261004-164359-yo9vna73`.

Capture: `/var/lib/barista-captures/20261004-164420-vmxwn__x`.
The observed host video span is 90.17 seconds. The log confirms the interval
experiment. All 58,505 encrypted observations examined authenticate using
two verified rotated PTKs. The host stream reconstructs 68 IDRs and 4,998 P
frames, plus one partial boundary frame; FFmpeg produces zero warnings.
IDR start spacing has minimum 233,568 us and median 266,929 us; two intervals
are below 250 ms and none below 200 ms, consistent with limiting encode
decisions while actual encoding/sending durations vary.

Radio analysis reports 4,752 complete observations, 308 incomplete, 4,712
fully Block-ACKed frames and 554 unique recovery requests. Format age median
is 1,813 us, p99 3,327 us and maximum 6,437 us, with none at least 8,000 us
old; median format-to-video lead is 5,078 us. These delivery and software
decoding results do not establish decoder acceptance.

The user reports **both more lag and more corruption**, tentatively. The
interval is **rejected**. The previously tested fixed-deadline engine was
restored immediately, and the interval gate and its production integration
were removed from source. The extra synthetic colored-transition coverage is
retained. This is evidence against retaining this particular cooldown, not
proof of why individual recovery messages occur.

After rollback, all 43 remaining CTest entries pass, including the expanded
colored-transition reconstruction. The rebuilt engine, installed engine and
pre-trial backup all match SHA-256
`bb21908b54d4726d595c6ffb3c739fd4eeaafedbd792592238469a341b13ee6a`.

## Packet-count-aware chunk spacing trial

The next hypothesis is that small multi-packet chunks should finish sooner,
so the decoder can consume them earlier. The authenticated real reference
has a median first multi-packet chunk duration of 642 us; the preceding
Barista run has 2,497 us. These are different scenes, and radio observations
include transmission delays, so this does not establish decoder starvation.
Large real-console IDRs can take longer than Barista's current windows.

Keep chunk starts at 0/3/6/9/11 ms. For each multi-packet chunk, use a span of
600 us per packet gap, capped at its existing completion window. A two-packet
first chunk now completes at 600 us instead of 2,500 us. Larger chunks retain
their previous schedule. Single-packet chunks, fixed frame deadlines,
encoder settings and recovery behavior remain unchanged. Focused schedule
tests cover small chunks and unchanged 44-packet transition frames.

Rollback binary: `/tmp/barista-engine-before-size-aware-spacing-20261004`.

All 43 CTest entries passed before installation. Built and installed engine
SHA-256: `b237386b2f5e5602a6f2e724b49a93e4a49546e4b460819f422c7774b0071c95`.
Runner logs: `/tmp/barista-experiment-20261004-165244-4gtbd19_`.

Capture: `/var/lib/barista-captures/20261004-165305-wlpq8_na`.
Support log: `run-20261004-225244-f068139f.log`, confirming the trial setting.
A connection retry left 48.78 seconds of active video. Host reconstruction
contains 91 IDRs and 2,552 P frames, plus one incomplete boundary frame;
FFmpeg produces zero warnings. Two-packet first chunks finish at median
596 us (30 samples); three-packet chunks at 1,197 us (12 samples).
The pacing change therefore took effect. There are 34 large frames; their
last-chunk maximum relative to the inferred deadline is 13,064 us.

Radio decryption authenticates 31,659 observations using one verified
rotated PTK; one observation fails authentication. Delivery analysis reports
2,457 complete radio frames, 165 incomplete, 2,430 fully Block-ACKed and
285 unique recovery requests. Format age median is 1,803 us, p99 3,320 us,
maximum 5,339 us, with none at least 8,000 us old. Median format-to-video
lead is 5,066 us. Duration and scene differences preclude comparing raw
recovery counts as equivalent trials.

The user reports **about the same** corruption and jitter. This change has
**no demonstrated visual benefit** and is reverted in source and the
installed engine. This tests earlier completion of small multi-packet
chunks; it does not eliminate all packet-timing hypotheses. Previous pacing,
fixed chunk deadlines and immediate recovery behavior remain the baseline.

After rollback, all 43 CTest entries pass. Rebuilt and installed engines
match the pre-trial backup SHA-256
`bb21908b54d4726d595c6ffb3c739fd4eeaafedbd792592238469a341b13ee6a`.

## Reopen recovery requests at resumed P start

The next trial shortens the recovery coalescing lifecycle to the start of
the first resumed P. Previously the sender cleared pending requests at that
frame's end, potentially discarding a request caused by that P. In the latest
capture, one of 288 decrypted request observations falls 10,758 us into one
of 91 resumed-P windows. Radio observation does not prove host arrival during
the gate or identify the decoder reason, but supplies a specific edge case.

Clear settling requests and reopen under the existing recovery mutex just
before first-P transmission. Retain the generation check so an older sender
cannot reopen a newer recovery. Requests during the resumed P remain pending
for the producer; no cooldown is introduced. Encoder, packet pacing and the
IDR / format-only / P sequence remain unchanged. A local mock-radio integration
test injects a request 5 ms into a resumed P and requires another IDR within
100 ms, before the next periodic request can account for it.

Rollback binary: `/tmp/barista-engine-before-resumed-p-start-20261004`.

All 44 CTest entries passed before installation, including the new request
injection test. Built and installed engine SHA-256:
`0ccc8fd6f5b8496aac21906da9b65e6c7163fa00e56ae5ca3a00d39912275eb3`.
Runner logs: `/tmp/barista-experiment-20261004-170055-jr3e0lf7`.

Capture: `/var/lib/barista-captures/20261004-170115-xk4fvqc0`.
Support log: `run-20261004-230055-edbf712b.log`, confirming the changed policy.
The bounded capture completed; observed host video spans 88.55 seconds.
The runner returned status 1 because Cemu exited normally (status 0) during
analysis. The saved session marks capture complete and its core reports were
already written. The missing Wi-Fi-rate summary was generated from the saved
radio capture, and the sanitized support log was saved as service diagnostics.
This was an analysis-stage runner interruption, not a truncated recording.

Host reconstruction yields 125 IDRs and 4,729 P frames, plus one incomplete
boundary frame. FFmpeg produces zero warnings. Radio delivery analysis
reports 57,537 authenticated observations using two verified rotated PTKs;
30 encrypted observations cannot be authenticated and are excluded. There
are 4,672 complete radio frames, 183 incomplete, 4,649 fully Block-ACKed and
448 unique recovery requests. Format age median is 1,810 us, p99 3,206 us,
maximum 4,978 us, with none at least 8,000 us old. Median format-to-video
lead is 5,060 us.

Of 124 resumed-P windows, 15 contain a request observation, each roughly
10.35–10.82 ms into the frame. The next IDR is observed 39.35–89.65 ms later.
Other requests can also account for those IDRs; capture ordering does not
establish the producer's exact event-consumption order. The local injection
test independently verifies the intended policy. There are 34 large host
frames; their last-chunk maximum relative to the inferred deadline is
13,068 us. All-frame first-packet lateness p99 is 795 us, maximum 3,056 us.
These are observations, not receiver execution measurements.

The user reports **“Seemed a bit better for both, still corruption but less
so?”** The change is **retained provisionally** for further comparison.
Different interactive scenes and a tentative visual report prevent claiming
a root-cause fix. Encoder settings, fixed chunk deadlines and previous packet
spacing remain intact. All 44 pre-install tests pass; rebuilt and installed
engine hashes still match the trial hash above.


## Scheduled format timestamp phase trial

The real reference's 3,282 unique formats have adjacent timestamp steps whose
error relative to the nearest 16,683.333-us frame-period multiple is at most
7.33 us, p99 6.33 us and median 0.33 us. The last Barista capture's 5,311
formats have median error 264.67 us, p99 1,057.67 us and maximum 2,552.33 us;
73 steps differ by at least 1 ms. Captured wall-clock steps themselves have
median error 3.33 us. Differences between timestamp steps and wall-clock
steps have median 278 us and p99 1,239 us, consistent with sampled clock
anchor changes contributing alongside scheduler delays. None of these
statistics is a measurement of the firmware display phase or a proven reset.

The trial uses intended format slot deadlines to advance a timestamp clock.
Project each observed TSF back by scheduler wakeup lateness, then correct the
clock's phase by at most 4 us per format. Elapsed time retains fractional
frame microseconds and missing-slot advancement. The 4-us bound is an
experimental tracking choice, not a recovered firmware requirement. Gradual
tracking can lag a genuine clock correction; inspect format age as well as
step consistency. First timestamp uses observed TSF; no new TSF epoch is
invented. Packet deadlines, format/video identity, PCM, native encoder and
resumed-P-start recovery are retained.

Deterministic tests cover alternating 1.5-ms observation jitter, slow clock
drift, missing slots and wire timestamp wrap. The runner also now permits
Cemu to close after ANALYZING starts without terminating capture analysis;
this changes workflow supervision only. Python syntax validation passes.
Rollback binary: `/tmp/barista-engine-before-format-phase-20261004`.
Visual outcome: roughly the same; reverted.

All 44 CTest entries passed before installation. Built and installed engine
SHA-256: `7cc2cc3ee3dd3f672bd9effacbf98339f094f06e69c789908b134b1b3d09945b`.
Runner logs: `/tmp/barista-experiment-20261004-170900-cli5jc7v`.

Capture: `/var/lib/barista-captures/20261004-170921-_fvtpoux`.
Support log confirms scheduled-slot phase, preset 5 and resumed-P-start
recovery. The runner completed normally with RESULT after recording and
analysis. Active host video spans 87.58 seconds. All 56,755 encrypted
observations examined authenticate with two verified rotated PTKs. Host
reconstruction contains 252 IDRs and 4,072 P frames, no incomplete frames;
FFmpeg produces zero warnings.

The 5,254 observed format timestamps have frame-step error median 3.67 us,
p99 and maximum 4.67 us, with no error at least 1 ms. The previous run's
maximum was 2,552.33 us and it had 73 errors at least 1 ms. The intended
cadence effect therefore occurred. Format age median is 1,828 us, p99
3,019 us and maximum 9,524 us, with two at least 8,000 us old. Median
format-to-video lead is 5,073 us. Smooth timestamp steps do not prove that
firmware phase checks passed or that all formats arrived on time.

Radio analysis reports 3,860 complete observations, 464 incomplete, 3,823
fully Block-ACKed and 918 unique recovery requests. The preceding run had
448 requests, but this run also has 174 large compressed frames versus 34
previously, and 252 IDRs versus 125. Content and encoding load differ;
these counters alone cannot establish a harmful or beneficial visual effect.
All-frame first-packet lateness p99 is 982 us and maximum 3,234 us. Large-frame
last-chunk maximum relative to inferred deadline is 14,449 us.

The user reports roughly the same video. The timestamp phase experiment is
reverted in source and the installed engine: smoothing the timestamp steps
did not demonstrate a visible benefit. The pre-trial resumed-P-start engine
is restored, and the phase clock and deadline callback changes are removed.
The separate runner analysis fix is retained.


## Skip discarded CAVLC residual serialization

The native encoder emits production CABAC through its macroblock callback,
then also runs the CAVLC residual writer for an output buffer that the adapter
discards. That extra entropy coding scales with nonzero coefficients, which
can increase during fades/slide-ins. The trial bypasses just that residual
serialization with an active CABAC context, retaining nonzero counts used by
neighbor and deblocking state. Ordinary CAVLC encoding is unchanged. The
remaining diagnostic scaffolding in CABAC mode is not a decodable stream.
Preset 5, QP32, prediction choices, recovery and packet pacing stay intact.
This is an encoding-headroom hypothesis, not a known decoder fix.

The prior engine with resumed-P-start recovery is backed up at
`/tmp/barista-engine-before-cavlc-residual-20261004`. The old reconstruction
probe is retained for direct old/new byte and pixel comparisons.

Direct old/new reconstruction comparison:
`/tmp/compare-cavlc-residual-trial.py`, results in
`/tmp/cavlc-residual-comparison.txt`. All CABAC bytes and reference pixels
match for 48 colored fade/slide frames with IDRs at 0/12/13/32, for both
preset 5 and offline fast-search coverage. Preset-5 combined output hash is
`47e4c5d877d5faed5364b5075c368ffc252b2bec4c6e1b12bd804acf9aac45e4`.
Fast-search coverage does not reintroduce that rejected setting for hardware.

The 600-frame benchmark before/after reports mean 30.8744/29.5740 ms,
p99 36.1678/33.5414 ms and output bytes 15,773,330 in both cases.
Both exceed the 16.6834-ms budget on this textured workload; a roughly 4.2%
mean reduction in a single sequential sample is not sufficient to guarantee
frame-budget compliance or physical improvement. Timing logs:
`/tmp/cavlc-residual-benchmark-before.txt` and
`/tmp/cavlc-residual-benchmark-after.txt`.

The first full suite passed 41/44 tests; three diagnostic reconstruction tests
failed because they deliberately decode CAVLC output alongside CABAC. No
experimental engine was installed at that point. The optimization now requires
an explicit run flag set by NativeEncoder; diagnostic callers retain complete
CAVLC by default. The corrected build is being rechecked before installation.

After the explicit native-only flag fix, the old/new byte and pixel comparison
passes again and all 44 CTest entries pass. Built and installed engine SHA-256:
`016e0d0715ce4b9bb41fb503e78dea24a974ecd861fc3d00d217c692d090888f`.
Runner logs: `/tmp/barista-experiment-20261004-171800-4j9pe34x`.

Capture: `/var/lib/barista-captures/20261004-171821-38ft2_xv`.
Support log: `run-20261004-231800-8a5d72da.log`, confirming the optimization,
preset 5, restored unsmoothed timestamps and resumed-P-start recovery.
The runner completes normally. Active host video spans 89.43 seconds.
All 58,487 encrypted observations examined authenticate with one verified
rotated PTK. Host reconstruction yields 90 IDRs and 4,993 P frames, plus one
incomplete boundary frame; FFmpeg produces zero warnings.

Radio analysis reports 4,672 complete frame observations, 412 incomplete,
4,641 fully Block-ACKed and 278 unique recovery requests. Format age median
is 1,803 us, p99 3,255 us and maximum 6,048 us, with none at least 8,000 us
old. Median format-to-video lead is 5,060 us. There are 28 large host frames;
last-chunk maximum relative to inferred deadline is 13,231 us. All-frame
first-packet lateness p99 is 692 us and maximum 2,177 us. Eighteen logged
encoding intervals have weighted mean about 10,902 us, 228 over-budget
frames and maximum 42,410 us. These figures describe this interactive run,
not a controlled comparison with the previous scene load.

The user reports **“Seemed like slightly less”** corruption. The change is
**retained provisionally**, supported by preserved encoded bytes/reference
pixels and a modest standalone timing improvement, without claiming the
root cause is fixed. Rebuilt and installed engine hashes still match the
trial hash above. The timestamp phase experiment remains reverted; the
separate runner analysis fix remains retained.


## Release build trial

All recent installed engines came from `build/`, whose cache explicitly sets
CMAKE_BUILD_TYPE=Debug. The retained native encoder's textured benchmark
remained around 30 ms/frame, over the 16.6834-ms frame budget. Test a Release
build of the same source before further codec or protocol changes. Use the
isolated Ninja directory `build-gamepad-release/`, retaining the Debug tree.
The existing patched hostapd build source is copied into this tree to avoid
network cloning; only the tested engine is installed for hardware playback.
The installed hostapd and service remain as before.

Verify Debug/Release CABAC bytes and reference pixels on the existing colored
transition workload, benchmark Release, and run its full CTest suite. The
runner now accepts --build-dir so its build step uses the selected tree.
The optional frontend path also follows that tree; this trial stays headless.
Rollback engine: `/tmp/barista-engine-before-release-build-20261004`.
Live result: Release retained for improved framerate; corruption remains.

The isolated Release build completes. `/tmp/compare-release-trial.py` confirms
identical CABAC bytes and every reference pixel on 48 colored transition
frames with consecutive IDRs, for default and offline fast-search coverage.
The preset-5 combined output hash remains
`47e4c5d877d5faed5364b5075c368ffc252b2bec4c6e1b12bd804acf9aac45e4`.
Results: `/tmp/barista-release-comparison.txt`.

The same 600-frame textured benchmark in Release reports mean 4.27773 ms,
p95 5.85654 ms, p99 7.62767 ms, maximum 8.77787 ms and zero frames over
16.6834 ms. Output bytes remain 15,773,330, matching the preceding Debug
benchmark, whose mean was 29.5740 ms. This is substantially more headroom for
this workload; it does not guarantee worst-case timing on every game frame.
Benchmark: `/tmp/barista-release-benchmark.txt`.

All 44 CTest entries pass in `build-gamepad-release` before installation.
Release build and installed engine SHA-256:
`f145adcb39c06044230410995010361b6d7982cbd9a72d4c1aab3cb21245d9c7`.
Runner logs: `/tmp/barista-experiment-20261004-172530-v5gfwprz`.
Command uses `--build-dir build-gamepad-release` with the same Cemu AppImage,
Wind Waker file, radio interfaces and 90-second window as prior trials.

Capture: `/var/lib/barista-captures/20261004-172550-rqa5b99w`.
Support log: `run-20261004-232530-0709526b.log`.
Active host video spans 88.57 seconds; observed frame cadence is 59.86 fps.
Host reconstruction yields 7 IDRs and 5,295 P frames, plus one incomplete
boundary frame; FFmpeg produces zero warnings. Eighteen logged timing
intervals contain 5,388 encoded frames with weighted average about 1,658 us,
maximum 6,978 us and zero frames over the 16.6834-ms budget. The log's frame
count includes periods outside the bounded host capture and is not the same
measurement as captured complete frames.

All 58,763 encrypted observations examined authenticate with one verified
rotated PTK. Radio analysis reports 4,966 complete observations, 337
incomplete, 4,933 fully Block-ACKed and 14 unique recovery requests.
Format age median is 1,798 us, p99 3,263 us, maximum 6,667 us, with none
at least 8,000 us old. Median format-to-video lead is 5,066 us. There are
9 large host frames; last-chunk maximum relative to inferred deadline is
13,478 us. All-frame first-packet lateness p99 is 801 us, maximum 3,052 us.

The user reports **“Corruption lingers and smears but framerate is much
better.”** Release is **retained for framerate and encoding headroom**.
The reduction from 278 to 14 recovery requests accompanies fewer IDRs and
a different interactive run; it is not a controlled causal comparison or a
count of visible corrupt frames. Lingering corruption/smearing remains
unresolved despite the faster encoder and software decode success.

All 44 Release tests pass, and Release/installed engine hashes match the
trial hash above. Further hardware changes must build, test and install from
`build-gamepad-release/`; keep the Debug tree for development. The runner's
explicit --build-dir selection and workflow documentation record this to
avoid accidentally restoring unoptimized hardware playback.


## Proactive reference reset after 60 frames (2026-10-04)

Status: retained for clearing lingering smear. Release remains the build baseline.
The user reports much better framerate but lingering corruption/smearing.
The last capture contained only 14 unique generic recovery requests despite
those visible artifacts. Hypothesis: a regular clean reference may bound
corruption that continues through P frames without requesting a reset.

The media producer forces an IDR at a frame-index distance of 60 from the
last successfully encoded IDR. Startup, source changes and requested IDRs
reset that distance; recovery requests retain their existing immediate
eligibility. Keep the format-only slot after each IDR and all packet/codec
settings. This adds IDR bandwidth and approximately one format-only slot
per second; it does not repair the underlying source of corruption.

The new mock-radio integration profile disables injected recovery requests
and requires at least two unsolicited IDRs exactly 60 frames apart after
startup, plus existing sequence, format-only-slot and chunk pacing checks.
The existing resumed-P request test continues checking prompt recovery.
Rollback engine: `/tmp/barista-engine-before-periodic-idr-20261004`,
SHA-256 `f145adcb39c06044230410995010361b6d7982cbd9a72d4c1aab3cb21245d9c7`.

Initial full suite: 44/45 passed; the new test incorrectly classified a
source-handoff IDR at frame 5 as periodic (observed IDRs: 0, 1, 5, 65, 125).
Restrict its exact-distance check to post-startup IDRs at frame 10 onward;
keep requiring at least two 60-frame resets and all packet/slot checks.

Corrected full Release suite: **45/45 passed**, 39.64 seconds. Installed
engine SHA-256: `612b7f13f8ace154adfd01d2e760363f56ef40772be4d620a5db2e34ba66e2f6`,
matching `build-gamepad-release/app/drcd`.
Runner: `/tmp/barista-experiment-20261004-173337-muvo42rf`.
Support log: `/var/log/barista/support/run-20261004-233337-e1edb2fb.log`.
The capture-ready bell rang, connection was confirmed, and the runner
launched Cemu 2.8 AppImage / Wind Waker unprivileged (PID 124108).

Capture: `/var/lib/barista-captures/20261004-173358-j7vgjwzm`.
Active video spans 85.563702 seconds. Host reconstruction: **4,969 complete
frames (160 IDR, 4,809 P)**, plus one boundary-incomplete frame. FFmpeg
reports zero warnings. Observed host cadence is **58.07 fps**, compared
with 59.86 in the previous Release run; the mandatory format-only slots
and more IDRs account for additional gaps. Frame-distance distribution
between observed IDRs: 3 (79 intervals), 9 (1), 20 (1), 24 (1), 59 (1),
60 (77). None exceed 60. Sixty-frame spacing establishes that proactive
resets reached the host stream; three-frame intervals show additional
recoveries, whose underlying reasons are not exposed by the generic request.

17 logged encode intervals cover 4,936 frames (different bounds from the
capture): weighted mean 1,562 us, maximum 8,483 us, **zero over-budget**
frames. Authenticated radio observations: 58,131, with one failed CCMP
observation excluded and two verified rotated PTKs. Radio reconstruction:
4,812 complete, 158 incomplete, 4,789 fully Block ACKed, **242 unique
recovery requests**. Previous Release had 14 requests; different interactive
content and added resets prevent assigning a single cause to this increase.
Block ACK is receipt evidence, not decoder acceptance.

Format age median 1,804 us, p99 3,270 us, maximum 19,817 us; two observations
reach 8 ms. Format-to-video lead median 5,069 us. Host first-packet lateness
p99 788 us, maximum 2,230 us. 50 large host frames (at least 15,000 payload
bytes), with last-chunk maximum 14,455 us relative to the inferred deadline.
These measurements do not identify the initiating corruption.

User result: **“Corruption still occers but does not linger and gets cleared.”**
Retain the periodic IDR as a recovery mitigation; corruption remains unresolved.
The user did not separately rate framerate. Keep the measured cadence and
extra requests visible as costs, and preserve the prior Release rollback.
Analysis artifacts: `/tmp/barista-periodic-idr.h264`,
`/tmp/barista-periodic-idr-decode.log` (empty),
`/tmp/barista-periodic-idr-deadlines.json`,
`/tmp/barista-periodic-idr-metrics.json`.


## Disable fractional motion search (2026-10-04)

Status: retained provisionally after tentative visual improvement. Preserve Release and the retained 60-frame proactive reset.
Hypothesis: a motion-prediction compatibility difference may initiate artifacts
in moving transitions, while IDRs clear the divergent reference chain. The
software reference tests passed earlier; this is not evidence of a known
hardware interpolation bug.

Add an opt-in MiniH264 create parameter to bypass fractional-pixel refinement
only. NativeEncoder selects it; zero-initialized diagnostic callers retain
normal behavior. Keep preset 5 integer search, skip prediction, QP32,
deblocking, packet timing and all recovery behavior. Starting from IDR zero
motion, integer search and median skip prediction retain integer luma vectors;
4:2:0 chroma can still have fractional predictions. Larger residuals and
quality changes are potential costs, to check alongside physical playback.

Rollback engine: `/tmp/barista-engine-before-integer-motion-20261004`,
SHA-256 `612b7f13f8ace154adfd01d2e760363f56ef40772be4d620a5db2e34ba66e2f6`.

Initial suite exposes an expected configuration mismatch in the default and
planar CABAC diagnostic probes: those compare bytes with NativeEncoder but
still selected fractional search. Set their diagnostic create parameter to
the same integer-motion policy, retaining full CAVLC and independent decoding
of both CAVLC and CABAC against every reference pixel. The separate production
worker/reconstruction comparison passes with the new policy.

Corrected full Release suite: **45/45 passed**, 38.83 seconds; initial
suite 43/45 passed before aligning the diagnostic motion configuration.
Installed engine SHA-256: `2951626814e899239c37a6fca70cf31b12ccd28fc85ed6c3421d4ef9ebe0f789`,
matching the Release build. Runner:
`/tmp/barista-experiment-20261004-174240-wa43p1ew`.
The ready bell rang, GamePad connected, and Cemu 2.8 / Wind Waker launched
unprivileged (PID 130160). Capture analysis and user result follow.

Support log: `/var/log/barista/support/run-20261004-234240-53ce2506.log`;
its live status confirms the selected integer-motion policy.
Capture: `/var/lib/barista-captures/20261004-174301-d0oitm4b`.
88.599474 seconds active video. Host reconstruction: **5,145 complete frames
(166 IDR, 4,979 P)** plus one boundary-incomplete frame; FFmpeg zero warnings.
Host cadence **58.07 fps**, essentially unchanged from the previous trial.
79 IDR intervals are exactly 60 frames; none exceed 60. Another 83 intervals
are three frames, continuing the additional-recovery pattern seen previously.

18 logged intervals cover 5,226 frames: weighted encoding mean **1,160 us**,
maximum 8,832 us, **zero over-budget** frames. Radio: all 59,050 encrypted
observations authenticated, two verified rotated PTKs, 5,078 complete frames,
68 incomplete, 5,064 fully Block ACKed, **249 unique recovery requests**
versus 242 previously. These counts alone do not indicate a visual improvement.

Format age median 1,794 us, p99 2,661 us, maximum 10,870 us, one observation
at least 8 ms; format-to-video lead median 5,061 us. Host first-packet lateness
p99 144 us, maximum 799 us, none at least 1 ms. Last chunk maximum 13,190 us.
24 large frames versus 50 previously; different scene content limits timing
and bitrate comparisons. Software validity does not establish physical
compatibility or isolate the source of corruption.

User: **“Hard to tell, seemed like an improvment tho, still corrupting.”**
Keep the integer-motion trial provisionally. This is a tentative visual benefit,
not proof that fractional prediction caused corruption. Corruption remains.
Preserve the rollback engine and avoid attributing scene-dependent timing
improvements to the motion policy alone.
Analysis artifacts: `/tmp/barista-integer-motion.h264`,
`/tmp/barista-integer-motion-decode.log` (empty),
`/tmp/barista-integer-motion-deadlines.json`,
`/tmp/barista-integer-motion-metrics.json`.


## Disable P-skip macroblocks (2026-10-04)

Status: retained provisionally. Keep all retained baseline behavior, including integer luma
motion and the 60-frame reset. The previous trial tentatively improved video,
but fades and sliding transitions still corrupt.

Hypothesis: approximate early skip omits small corrections during fades, or
skip prediction on the physical decoder differs from our reference. Neither
is established as the initiating fault. Add an opt-in create parameter that
prevents early skip selection and post-quantization zero-residual conversion
to skip. Preserve skip motion as a regular explicit inter candidate, other
search/quantization, deblocking and protocol settings. NativeEncoder and its
byte-comparison diagnostic select the flag; normal diagnostics leave it zero.
Independent CAVLC and CABAC decoding must still match reference pixels.
Expect extra residual analysis, explicit macroblock bits and potential changes
in reconstruction/source fidelity; measure encoder load and packet sizes.

Rollback engine: `/tmp/barista-engine-before-no-pskip-20261004`, SHA-256
`2951626814e899239c37a6fca70cf31b12ccd28fc85ed6c3421d4ef9ebe0f789`.

Full Release suite: **45/45 passed**, 38.68 seconds. Independent 48 moving
frames and 120 fade frames match every reference pixel for both presets.
The synthetic fade sampled luma MSE is unchanged at 19.9083; this does not
improve that software source-fidelity result. Installed engine SHA-256:
`1042fbc0ebc2d444927303d6126dbf0af506c385612e6bf4912981d05e66db99`,
matching the Release build. Runner:
`/tmp/barista-experiment-20261004-175856-exlvzuvg`.
Capture-ready bell rang, connection confirmed, and Cemu 2.8 / Wind Waker
launched unprivileged (PID 139149). Results follow.

Support log: `/var/log/barista/support/run-20261004-235856-6b63ef2c.log`;
live status confirms P-skip is disabled. Capture:
`/var/lib/barista-captures/20261004-175917-8xmqn001`.
87.598477 seconds active video, **5,088 complete host frames (163 IDR,
4,925 P)** plus one boundary-incomplete frame. FFmpeg reports zero warnings.
Host cadence **58.08 fps** versus 58.07 previously. 79 IDR intervals have
exactly 60 frames, none exceed 60; 81 additional intervals are three frames.
The recurrent additional-recovery pattern remains.

18 logged encode intervals cover 5,228 frames, with weighted encoding mean
**1,349 us**, maximum 8,505 us, **zero over-budget** frames. Previous mean
was 1,160 us with different game content; extra explicit coding is a possible
cost but this is not a controlled workload comparison. All 57,805 encrypted
radio observations authenticate; two verified rotated PTKs. Radio has 4,839
complete frames, 249 incomplete, 4,801 fully Block ACKed and **243 unique
recovery requests**, compared with 249 previously. Sniffer incompleteness
is not proof of receiver loss; request counts are not visual failure counts.

Format age median 1,792 us, p99 2,645 us, maximum 17,866 us; one observation
reaches 8 ms. Format-to-video lead median 5,059 us. Host first-packet lateness
p99 75 us, max 417 us, none at least 1 ms; last-chunk maximum 13,062 us.
16 large host frames versus 24 previously; scene differences prevent assigning
the packet-size/timing change to P-skip policy. Host/software success does
not establish GamePad compatibility.

User: **“Seems like an improvement but still corrupting in the same spots.”**
Retain no-P-skip provisionally. This is a tentative visual improvement, not a
root-cause fix; the synthetic fade source error and recurring artifact spots
remain. Preserve the prior integer-motion Release engine for rollback.
Analysis artifacts: `/tmp/barista-no-pskip.h264`,
`/tmp/barista-no-pskip-decode.log` (empty),
`/tmp/barista-no-pskip-deadlines.json`, `/tmp/barista-no-pskip-metrics.json`.


## Zero-motion inter prediction (2026-10-04)

Status: retained after strong visual improvement. Retain Release, no-P-skip, restricted planar prediction,
fixed QP32, 60-frame resets and all protocol settings.
Hypothesis: residual motion/chroma interpolation exposes a physical decoder
reference difference. Integer luma motion can still imply fractional chroma
prediction. This is not a demonstrated firmware or encoder defect.

NativeEncoder selects `zero_motion_flag`; diagnostic comparison selects the
same setting, and ordinary callers default to normal motion search. The
inter candidate uses absolute zero motion, an MVD cancelling its predictor,
and the same-position reference pixels. Preserve intra selection, residual
quantization, deblocking and reference reconstruction. IDRs are unchanged.
This removes useful motion compensation and can increase moving-scene
residuals, radio load or source error. Exact CAVLC/CABAC reference comparisons
and the full Release suite precede hardware testing.

Rollback engine: `/tmp/barista-engine-before-zero-motion-20261004`, SHA-256
`1042fbc0ebc2d444927303d6126dbf0af506c385612e6bf4912981d05e66db99`.

Full Release suite: **45/45 passed**, 38.28 seconds. Installed engine
SHA-256 `d9a00f67397d2ee020e67f9a1e874c0d2caa852b3e04154c8901416031a91de8`,
matching the Release build. Runner:
`/tmp/barista-experiment-20261004-180541-nqdg6iws`.
Ready bell rang, connection confirmed, and Cemu 2.8 / Wind Waker launched
unprivileged (PID 143662). Results follow.

Support log: `/var/log/barista/support/run-20261005-000541-9e449f47.log`;
live status confirms zero-motion same-position prediction. Capture:
`/var/lib/barista-captures/20261004-180602-jo9dg6o_`.
Active video **88.716266 seconds**, host reconstruction **5,148 complete
frames (170 IDR, 4,978 P)** plus one boundary-incomplete frame. FFmpeg zero
warnings. Cadence **58.02 fps**, compared with 58.08 previously. Seventy-five
IDR intervals are 60 frames, none exceed 60; 83 additional intervals are
three frames, so the repeated additional-recovery pattern persists.

18 logged intervals cover 5,223 frames: weighted encoding mean **1,271 us**,
maximum 9,089 us, **zero over-budget** frames. All 59,339 encrypted radio
observations authenticate, one verified rotated PTK. Radio reconstruction:
4,948 complete frames, 200 incomplete, 4,926 fully Block ACKed, **256 unique
recovery requests** versus 243 previously. These counts did not track the
large visual improvement. Missing sniffer packets are not proven pad loss;
Block ACK is MAC receipt, not decoder acceptance.

Format age median 1,794 us, p99 2,657 us, max 18,791 us; two observations
at least 8 ms. Format-to-video lead median 5,060 us, with extreme observed
values 2 and 34,277 us; nearby beacon/matching outliers do not establish
actual decoder deadlines. Host first-packet lateness p99 92 us, max 774 us,
none at least 1 ms. Last-chunk max 14,034 us; 22 large frames versus 16
previously. Encoded host video payload is 5,195,629 bytes, approximately
0.469 Mbit/s over the observed span, excluding protocol overhead. Different
interactive content prevents a controlled bitrate or source-quality comparison.

User: **“Omg, its pretty much all gone.”** Retain zero-motion prediction as
the new physical comparison baseline; this is the strongest visual result
so far, not a claim that every artifact is eliminated. The result points
toward motion-compensated prediction, but simultaneously avoids nonzero vector
coding, spatial predictor interactions and interpolation. It does not isolate
which of those paths is responsible. Future tests should preserve this working
engine and vary one of those paths with deterministic reference checks.

Saved working engine: `/tmp/barista-engine-zero-motion-retained-20261004`,
SHA-256 `d9a00f67397d2ee020e67f9a1e874c0d2caa852b3e04154c8901416031a91de8`.
Previous no-P-skip engine remains available for rollback.
Analysis: `/tmp/barista-zero-motion.h264`,
`/tmp/barista-zero-motion-decode.log` (empty),
`/tmp/barista-zero-motion-deadlines.json`, `/tmp/barista-zero-motion-metrics.json`.


User follow-up for the same zero-motion trial: **“I saw basically 0 corruption
that time.”** This strengthens the recorded physical result from “pretty much
all gone” to essentially zero observed corruption in that run. Keep the saved
zero-motion engine as the comparison baseline. This is a playback observation
for this bounded session, not proof of the exact underlying defect or a claim
that all future content will be artifact-free.
