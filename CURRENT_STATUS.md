# Wii U GamePad Host: Current Status

Last updated: 2026-09-05

## Isolated encoder test: cyclic intra-refresh off

User's artifact.MOV shows intermittent colored blocks near the upper image,
interspersed with visually intact frames. This is not established to be
ordinary quantization loss. No full encoder replacement is justified yet.

Added `DRCD_INTRA_REFRESH=0` to disable only x264 cyclic intra-refresh. Default
remains enabled. Requested IDRs and all other encoder parameters, QP32,
send-time timestamps, chunk pacing and the serial sender remain unchanged.
Runtime logs identify the selected mode. Dedicated core and loopback tests
exercise the option with repeated recovery requests and packet checks.
Physical compatibility/artifact improvement remains unverified.

```sh
sudo env DRCD_CEMU_SOCKET=/tmp/drcd-media.sock DRCD_INTRA_REFRESH=0 ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-no-intra-refresh.pcap --np
```

Omit DRCD_INTRA_REFRESH to return to the prior baseline.

## Encoding and paced transmission now overlap

The first chunk-paced hardware run regressed to 50fps with frame gaps up to
91.5ms. It encoded and sent on one worker, so ~11ms spent pacing one frame
delayed starting the next encode. User also reports unpaced mode remains
unstable, though less so; this scheduling fix is not proof of solved recovery.

Added a dedicated serial sender with a bounded handoff: one sending frame,
one next frame encoding/waiting, and no additional pending queue. Frame order
and reference continuity are preserved; no encoded frames are dropped. The
sender alone owns pacing deadlines, stamps frames just before sending, and
drains/joins before video-loop state is destroyed. Sender task exceptions stop
media rather than silently continuing a broken reference chain.

QP32, AP clock auto-selection, IDR init, chunk offsets, audio and keepalive
settings remain unchanged. Chunk pacing remains enabled by default; clear any
previous DRCD_CHUNK_PACING=0 override for this test:

```sh
sudo env DRCD_CEMU_SOCKET=/tmp/drcd-media.sock ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-pipelined.pcap --np
```

Added deterministic sender tests for concurrent producer progress, bounded
handoff, FIFO order, shutdown draining and exception handling. Loopback tests
also check packet sequence continuity and existing chunk timing/audio/input.
Physical smoothness and decoder recovery remain pending.

## Chunk pacing implemented; working options now defaults

The stable `/tmp/vanilla-real-live.pcap` has 59.94fps, zero recovery requests,
no internal video sequence gaps and no IDRs during the minute. Real P-frame
chunk endings are ~0/3/5.8/8.8/11ms. The earlier real-console dump's six IDRs
have median chunk endings ~2.5/4.9/7.4/10.3/13.1ms, consistent with spreading
larger chunks across the frame rather than sending all chunks at once.

Implemented chunk-start deadlines 0/3/6/9/11ms from frame send start, with
packets within each chunk contiguous. All chunks retain the same timestamp;
no frames are encoded then discarded. PCM/input run independently. Slow
encoding or WLAN queueing can still exceed the target cadence; currently the
video worker performs both encoding and paced sending. Hardware result pending.

Defaults now include QP32, send-time timestamps, init on every IDR and chunk
pacing. Extra post-IDR pause stays OFF, as do fast encoding and reference-option
reordering. Actual AP TSF auto-selects only on the verified rtw89_8852be driver;
startup fails if selected hardware-clock reads fail. Loopback and other drivers
retain their existing clock paths. Explicit environment overrides remain.

```sh
sudo env DRCD_CEMU_SOCKET=/tmp/drcd-media.sock ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-chunk-paced.pcap --np
```

No AP-clock/QP/send-time/init/pacing switches needed. Cemu still uses its usual
CEMU_DRCD_SOCKET launch setting. DRCD_CEMU_SOCKET remains explicit to select
Cemu rather than file/generated input. Unset previously exported experiment
overrides. Use DRCD_CHUNK_PACING=0 to compare against unpaced chunks while
retaining the other new defaults.

Loopback regressions cover chunk offsets, identical timestamps across chunks,
repeated IDR flags, video age, audio/input continuity, optional IDR pause and
explicit legacy defaults. Earlier sections below are historical experiments.

## Post-IDR spacing test implemented

User reports init-bit test remains smooth but heavily artifacted. Its capture
is `/tmp/drcd-cemu-idr-init.pca-ip.pcap`: 2,809 complete frames, 1,621 recovery
requests, one three-packet sequence gap near 8.247s alongside three logged
send errors. All captured IDR/P init flags match the requested policy.
Reconstructable video still decodes without FFmpeg errors; init alone did not
establish recovery.

Added opt-in `DRCD_IDR_PAUSE=1`: schedule the frame AFTER each actual IDR for
two frame intervals (~33.366ms) after that IDR's actual send start. Other frame
intervals remain ~16.683ms. No encoded frames are discarded, and PCM/input
workers remain independent. A slow encode can still exceed the deadline.
Repeated IDRs can therefore lower output rate toward 30fps; this is deliberate
for the compatibility test, not a global 30fps mode. Default unchanged.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_CEMU_SOCKET=/tmp/drcd-media.sock DRCD_VIDEO_QP=32 DRCD_SEND_TIME_VIDEO=1 DRCD_IDR_INIT=1 DRCD_IDR_PAUSE=1 ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-idr-pause.pcap --np
```

The new recovery-spacing loopback test checks repeated IDR requests, every
packet's init flag, post-IDR versus post-P pacing, fresh video timestamps,
PCM flow and input forwarding. Physical artifact/recovery outcome pending.
Omit only DRCD_IDR_PAUSE to restore the preceding init-bit test.

## Recovery flag mismatch: isolated IDR-init test ready

Authenticated Mac capture analysis found 1,754 fully Block-ACKed keyframes;
1,712 were followed by another recovery request within 5–50ms of full ACK.
This confirms MAC receipt, not decoder acceptance or which later frame failed.

Compared every VSTRM packet in the real-console recovery sequences with
`/tmp/drcd-cemu-send-time-2-ip.pcap`. The real console sets byte 2 bit 0x80
(historically named init) on ALL packets of 5/6 captured IDRs, including late
recovery IDRs at 16.190s, 33.726s and 36.095s. The single unflagged IDR at
15.906s was followed by another request cluster at 16.153s. Its next IDR was
flagged. drcd previously set the bit on only its first session frame, following
the old libdrc implementation. Bit semantics and causation remain unproven.

Added opt-in `DRCD_IDR_INIT=1`: set that bit on every packet of every actual
IDR; leave P packets unchanged. No change to encoder, QP, sequence continuity,
other options, PCM or keepalive. Keep the previously improved send-time profile:

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_CEMU_SOCKET=/tmp/drcd-media.sock DRCD_VIDEO_QP=32 DRCD_SEND_TIME_VIDEO=1 DRCD_IDR_INIT=1 ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-idr-init.pcap --np
```

Unit policy assertions and a loopback test inject repeated recovery requests
and verify the init bit on ALL IDR packets and its absence on P packets.
Hardware outcome pending. Omit only DRCD_IDR_INIT to restore the preceding test.

Another observed difference, deliberately NOT changed in this test: each real
IDR is followed by the next captured frame at a timestamp delta ~33,367us
instead of the usual ~16,683us. Capture omissions versus deliberate decoder
spacing need further investigation if the flag test is insufficient.

## Recovery investigation: full-stream decoding passes; send-time test ready

QP36 retries 2/3 failed physically despite changing outgoing keyframes.
Added `scripts/reconstruct-media-capture.py`, using Vanilla's receiver slice
headers and existing SPS/PPS to reconstruct complete IDR/P chains. It skips
incomplete frames and waits for the next complete IDR after a broken chain.
FFmpeg decoded QP36 run 3's 974 IDRs + 805 P frames with no errors; Cemu5's
1,774 IDRs + 11,264 P frames also passed. The real-console capture yielded
6 IDRs + 699 P frames without decode errors; 20 incomplete captured frames
prevented reconstructing the other chains. This is software-decoder evidence,
not proof of physical receiver compatibility or wireless delivery.

Real-console requests occur in four clusters, with the next IDR 3.8–38.6ms
later. Our failed run's median next-IDR delay was similar (~19ms vs ~22ms),
so simply responding faster is not established as the missing recovery step.

Beacon-corrected reference video age is median 6,269us (p10 6,055, p90 6,435).
Our baseline timestamps precede encoding AND the pacing sleep, adding roughly
one frame period or slow-encode duration to age before transmission. Added
opt-in `DRCD_SEND_TIME_VIDEO=1`: restamp format and every video packet AFTER
encoding and sleep, just before send, at clock-6250us. PCM unchanged. Unlike
the historical reverted timing experiment, this one stamps after pacing sleep
and is intended to run with the now-working actual AP TSF clock.

Default remains unchanged. Use QP32, baseline encoding and baseline options:

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_CEMU_SOCKET=/tmp/drcd-media.sock DRCD_VIDEO_QP=32 DRCD_SEND_TIME_VIDEO=1 ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-send-time.pcap --np
```

Timestamp tests cover endian layout, wraparound, unmodified payload/flags and
loopback receive-age checking. Hardware recovery remains unverified; radio
queue delay after send and physical decoder behavior are still candidates.

## QP 36 test ready after QP 28 regression

User reports QP 28 was much worse. Added `DRCD_VIDEO_QP=28|32|36`;
this takes precedence over the legacy `DRCD_QP28=1` option. Unset both
for the original QP 32; invalid DRCD_VIDEO_QP values also select 32.
QP 36 tests stronger compression, trading detail for smaller frames.
No other streaming settings changed; hardware outcome remains pending.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_CEMU_SOCKET=/tmp/drcd-media.sock DRCD_VIDEO_QP=36 ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-qp36.pcap --np
```

## QP 28 comparison available; baseline remains QP 32

User confirmed the software keyboard and smoother WWHD playback in capture 5.
Added `DRCD_QP28=1` to select fixed QP 28 instead of 32; unset it to restore
the baseline. Runtime logs report the selected QP. No other encoder, clock,
packet, input or keepalive settings changed. Physical quality/performance
comparison pending; lower quantization can increase packet sizes and CPU work.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_CEMU_SOCKET=/tmp/drcd-media.sock DRCD_QP28=1 ./scripts/capture-drcd-session.sh /tmp/drcd-cemu-qp28.pcap --np
```

## Cemu keyboard overlay ready for physical test

User reports much better input in the longer `/tmp/drcd-cemu3.pcap` run.
Its IP capture has 6,844 complete video frames and 14,057 PCM packets;
whole-run video rate is about 56.2fps. Cemu logged keyboard appearance at
22:48:47.497; host video remained 59–60 frames per second from 22:48:44
through 22:48:51. That rules out a host sending halt there, not stale source
frames, receiver loss or decoder stalls.

The bridge captured the guest DRC texture before Cemu drew its software
keyboard. While the keyboard is active, Cemu now rasterizes the already
evaluated main-window ImGui overlay into the stream on a dark background,
instead of capturing raw DRC scanouts. This does not invoke keyboard callbacks
again or require the game to refresh its DRC texture. Normal DRC capture resumes
after keyboard exit. Both OpenGL and Vulkan call this path. The main window
must continue rendering; touch input remains unimplemented.

Also corrected legacy ImGui navigation setup: declare gamepad availability
and clear navigation inputs each frame so releases do not remain held.
Keyboard controls use D-pad/A, B for backspace and Plus for confirmation.
No daemon, encoder, packet, AP clock or keepalive changes in this test.

Built Cemu_debug and CemuDrcdOverlayTests successfully; overlay fixtures cover
clipping, alpha, glyphs, ignored textures, translated display origin and
navigation press/release. Tests pass; a synthetic dense keyboard render took
about 9.6ms locally (not an end-to-end latency measurement). Git diff check
passes. Physical keyboard display/navigation and exit recovery are pending.
Restart Cemu with the usual CEMU_DRCD_SOCKET setting; drcd can remain running.

## Input decoding and encode-coupled forwarding fixed

`/tmp/drcd-cemu2-ip.pcap` contains 13,495 input reports. Arrival spacing:
median 5,563 us, p99 10,224 us; 36 gaps exceed 25ms (largest 756ms near
75.6s, also 138ms near 13.8s). These are host-capture arrival times, not
button-to-game latency, and do not by themselves identify radio loss.

Found two Cemu adapter bugs left over from the old provider: main button
bytes 2–3 were decoded little-endian, but are big-endian; X/Y masks were
reversed. Corrected both, matching drcd and Vanilla's packet implementation.
Stick fields remain little-endian. Compile-time independent wire fixtures
cover each of the 18 physical buttons (including high-byte L3/R3/TV).

Moved drcd's IPC input forwarding out of the video encoding loop into its
own 1ms-poll worker, so long IDR encodes no longer gate forwarding. Worker
joins before destroying IPC; existing bounded/latest-report behavior remains.
Local IPC worker/socket scheduling and pre-daemon arrival gaps can still add
latency. No video, audio, clock, encoder settings or saved profiles changed.
Cemu and drcd rebuilt successfully; all seven daemon/IPC tests pass.
Restart both processes for these changes; physical input acceptance pending.

## Physical Cemu logo/game confirmed; input profile corrected

User confirms idle Cemu logo and actual GamePad game view displayed well,
with some frame drops/artifacts. `/tmp/drcd.log` confirms IPC connected,
GamePad active, idle→game and disconnect→idle. Keep video unchanged for input
testing. Complex game scenes still elicited recovery requests/encode overruns;
later windows ran 300 frames per five seconds without new recovery requests.

Input blocker found in `~/.config/Cemu/controllerProfiles/controller0.xml`:
physical WiiUGamePad had only HOME and TV mappings. Other buttons/sticks were
still mapped to Steam Virtual Gamepad. Cemu's default mapping fills only empty
logical controls, so adding the physical device did not replace existing ones.

Backed up original profile to
`checkpoints/cemu-controller0-before-drcd-input-20260904.xml`. With Cemu closed,
assigned all 26 supported controls to WiiUGamePad and cleared conflicting
DSU/SDL mappings (device/settings entries preserved). Saved both controller0
and named `DRCD GamePad.xml` in the user's controllerProfiles directory.
Validated both XML files, all 26 controls and absence of duplicate mappings.
No Cemu/drcd source or streaming settings changed for this profile-only fix. Restart Cemu
with the same socket option; choose DRCD GamePad if another profile overrides
controller 1. Physical control acceptance remains pending. Touch/motion/rumble
are still unimplemented.

## Current work: Cemu migrated to standalone drcd media/input IPC

At user request, inspected Cemu's dirty Git tree and removed its old native
pairing GUI, hostapd/DHCP build helpers, network cleanup and direct UDP transport.
Source backup: `checkpoints/cemu-before-drcd-20260904.tar.gz`; removed directories
and helper binaries also preserved at `/tmp/cemu-drc-migration-PoxSUN/`.
System polkit policy files were not touched. No pairing credentials migrated
into Cemu: drcd remains the sole owner of pairing and wireless state.

Added `libdrc-ipc`: Linux local authenticated Unix sequenced-packet connection,
bounded latest-frame and PCM queues, RGB→I420 conversion, idle image storage,
activity heartbeat, input expiry and reconnect. The server accepts only the
sudo-invoking UID (or daemon UID), or root; socket permissions 0600. Stale
socket files are not automatically deleted. Media frames are published only
when complete. Malformed packet tests and existing-endpoint protection pass.

Cemu client uses its existing logo asset while idle; hooks the original DRC
render target before TV/pad-window swapping and independent of pad-window
visibility. OpenGL/Vulkan readback feeds the same proven drcd slow encoder.
GamePad PCM and buttons/sticks travel over IPC. Stopping a title returns to
the logo; client disconnect silences audio and expires input. Touch/motion/
rumble remain unimplemented in the replacement input adapter.

All seven drc-project tests pass, including an end-to-end local media-stream
test: 179 frames, 347 PCM packets in 3 seconds, nonzero audio verified and
input returned through the bridge. Cemu's final build compiled and linked
successfully, including the direct-capture render-pass guard. Physical logo/game rendering
is pending. GPU readback is synchronous and may add GPU stalls; this is not yet
a validated low-latency Cemu streaming implementation. No encoder/clock/keepalive
compatibility settings changed. File/generated tests remain available.

Full commands and limitations: [Cemu integration](docs/cemu-integration.md).

From drc-project:

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_CEMU_SOCKET=/tmp/drcd-media.sock ./scripts/capture-drcd-session.sh /tmp/drcd-cemu.pcap --np
```

From workspace root, **without sudo**:

```sh
env CEMU_DRCD_SOCKET=/tmp/drcd-media.sock ./Cemu/bin/Cemu_debug
```

Do not enable `--play`, `--black`, or experimental encode/header options.

## Ready: native generated A/V, no media file or FFmpeg

User reports test2 also choppy. `/tmp/drcd-test2-ip.pcap` has 1,544 complete
outgoing frames, 49.87 fps, maximum frame gap 42.28 ms. PCM median 8,666 us;
logged send errors/timeouts zero. Log confirms `./test2.mp4` was selected.
This makes a problem specific to test1 less likely; it does not rule out
shared decoding/encoding/timing problems or Wi-Fi loss. Radio capture is
only 279 KB versus 25 MB of IP traffic and cannot establish full-run delivery.

`DRCD_GENERATED_AV=1` with `--black` now generates eight grayscale bars,
a moving bright marker, and a quiet stereo 440 Hz tone directly in drcd.
No file is opened and neither video nor audio invokes FFmpeg. The original
slow x264 encoder, AP-clock option, packetization, recovery and scheduling
remain unchanged. Plain `--black` still means black video without PCM;
file playback is unaffected by the new variable. Generator motion and tone
phase/stereo checks added; build and all five tests pass.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_GENERATED_AV=1 ./scripts/capture-drcd-session.sh /tmp/drcd-generated-av.pcap --np --black
```

Keep fast encode and reference-option-order experiments off. Run about 60 s.
This intentionally simpler scene also reduces encoding cost and bandwidth;
success would not alone distinguish those factors from bypassing FFmpeg.

## Ready: isolated reference video-option order test

Recovery-cost hardware capture stayed active after 6.962 s. All 2,244
outgoing frames were complete, average 47.53 fps; worst frame gap 46.34 ms.
One five-second window had 141 IDRs averaging 34.26 ms versus six P frames
averaging 3.82 ms. PCM median spacing was 8,666 us, maximum 10,366 us;
logged send errors and command timeouts remained zero.

The reference IP capture has 12 recovery requests and 2,007 frame starts;
our recovery-cost capture has 2,653 requests and 2,244 frame starts.
Reconstructed all complete, sequence-contiguous IDRs using the existing
fixed SPS/PPS/slice-header reconstruction, concatenated their raw chunk
payloads with emulation prevention, and decoded via FFmpeg: 6 reference,
1,355 recovery-cost, and 303 fast-encode IDRs all returned zero without
warnings. This only checks offline IDR decodability, not P-frame continuity,
GamePad decoder acceptance, radio delivery, or presentation deadlines.
Local radio capture has no AP beacons; it cannot independently establish
AP-clock media age or actual retransmission/loss rates.

Next isolated hypothesis: option ordering may affect GamePad processing.
`DRCD_REFERENCE_VIDEO_OPTIONS=1` changes only bytes 8–15 of VSTRM headers:
IDR `8082008385060000`, non-IDR `8200838506000000`, matching authenticated
console packets. Default order stays unchanged. Encoder search, recovery
responses, timestamps, audio and keepalive are untouched. Both option
fixtures and unchanged surrounding header/payload bytes are regression-tested.
Build and all five tests pass. Hardware acceptance remains unverified.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_REFERENCE_VIDEO_OPTIONS=1 ./scripts/capture-drcd-session.sh /tmp/drcd-reference-options.pcap --np --play ./test1.mkv
```

Run for about 60 seconds, then Ctrl-C. Omit only the new option to restore
the prior working behavior; keep the AP clock enabled and fast encode off.

## Current build: recovery-cost diagnostics; playback settings preserved

A 30-second loopback run of the unchanged slow encoder with no injected
resync requests produced 1,793 frames at 59.85 fps and 3,459 PCM packets.
Five-second encode averages were 3.63–5.92 ms (occasional maxima up to
41.62 ms). The prior every-frame-resync benchmark achieved only 47.30 fps.
This isolates a substantial recovery-workload cost without Wi-Fi; it does
not explain why the physical GamePad keeps requesting recovery.

The x264 build already uses O3 and x86 assembly. No encoder preset, clock,
packet format, frame schedule or keepalive changes were made. Added only
five-second diagnostics: actual IDR/P frame counts and mean encode costs,
encode attempts exceeding 16,683 us, and consumed resync events. Events
coalesce multiple requests, so their count is not the raw incoming count.
Build and all five CTest tests pass.

Repeated-IDR verification with the new diagnostics reproduced 47.00 fps:
one five-second window had 145 IDRs averaging 34.19 ms, all over budget.
No encoding errors occurred; benchmark exit 1 indicates its >=55 fps
threshold was missed. FFmpeg broken-pipe output is from intentional shutdown.

Next hardware capture: retain the working AP clock and slow encoder, run
for at least 60 seconds including playback/choppiness, then stop with Ctrl-C.
Do not enable `DRCD_FAST_ENCODE`. Preserve `/tmp/drcd.log` before another run.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 ./scripts/capture-drcd-session.sh /tmp/drcd-recovery-cost.pcap --np --play ./test1.mkv
```

These diagnostics are not a playback fix. Do not suppress recovery requests
or change the proven bitstream settings based solely on loopback throughput.

## Hardware follow-up: retain slow encoder with AP clock

User confirms `DRCD_AP_TSF_CLOCK=1` still produces playback, while adding
`DRCD_FAST_ENCODE=1` did not. Do not promote fast search based on its loopback
benchmark or protocol self-test: neither establishes GamePad decoder acceptance.
Default encoding remains unchanged slow search; use only the AP-clock option.

`/tmp/drcd-ap-tsf-2-ip.pcap` shows waiting→active at 18.89 s, with 1,345
active input observations. `/tmp/drcd-fast-encode-ip.pcap` has 1,558 waiting
observations and no active transition. The fast capture spans about 15.4 s
between first/last frame starts, includes a ~1.03 s media gap, sequence resets
and near-duplicate frames. It is not one uninterrupted clean A/B trial.
Video-vs-PCM timestamp medians are similar: -16.456 ms slow versus -16.444 ms
fast. No obvious median clock change explains the differing outcome.

Fast search remains an unvalidated experimental option, not a recommended
playback setting. The successful checkpoint remains available. No further
encoder/clock changes were applied after this hardware feedback. Further
performance work must retain proven bitstream settings or separately establish
hardware acceptance; do not infer compatibility from faster local output.

## Ready: optional fast encoder search (AP clock preserved)

Thirty-second loopback benchmarks with `test1.mkv` and an IDR request on every
received frame reproduced the frame-rate bottleneck independently of Wi-Fi:

| Search mode | Frames | Measured fps | Observed video payload rate |
| --- | --- | --- | --- |
| Existing slow | 1417 | 47.3046 | 6.067 Mbps |
| Fast search | 1796 | 59.9412 | 7.829 Mbps |

Slow-mode five-second encode averages reached 33,672 us, max 35,480 us,
exceeding the 16,683-us frame budget. Fast-mode interval averages were
1,677–3,717 us, max 5,890 us. Both delivered 3,459 PCM packets. The bitrate
comparison includes different numbers of frames/content progress and is not
a matched-frame compression-quality test. Benchmark exit code 1 for the
baseline means its >=55fps threshold failed, not a crash. FFmpeg broken-pipe
messages occur when the benchmark stops consuming its output.

`DRCD_FAST_ENCODE=1` changes only motion search to DIA, subpixel refinement
to 2, and trellis to 0 after the original slow preset. All explicit DRH/CABAC,
reference-frame, chunking, QP, packet-format and scheduling constraints remain.
Actual AP clock and keepalive behavior are untouched. The default still uses
baseline search. Added encode timing logs and a second media protocol self-test
under the fast option. The prior playback checkpoint remains untouched.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 DRCD_FAST_ENCODE=1 ./scripts/capture-drcd-session.sh /tmp/drcd-fast-encode.pcap --np --play ./test1.mkv
```

GamePad playback with faster search is not yet verified. If it regresses,
omit only `DRCD_FAST_ENCODE=1`, retaining `DRCD_AP_TSF_CLOCK=1`. Do not run
the register probe concurrently with actual AP-clock mode.

## CONFIRMED PLAYBACK with actual AP TSF (choppy)

User reports playing video plus audio with `DRCD_AP_TSF_CLOCK=1`; Mac capture
is `../mac-working.pcap`. `/tmp/drcd-ap-tsf-ip.pcap` confirms waiting→active
at 12.278 s and no later waiting transition through 52.219 s. 46 UVC/UAC
reply observations, max adjacent gap 1.071 s; no PCM/video timestamp
regressions. All 2,100 outgoing video frames are complete. Actual aggregate
frame rate 44.96 fps; frame gaps reach 41.21 ms. PCM median 8,667 us.
Five-second video-production buckets range from about 30 to 60 fps.
IDR requests persist heavily, so active status is not proof every frame decodes.

Mac usable prefix covers ~49.87 s with 463 target beacons and 40,607 AP data
observations, 6,591 carrying the retry bit. This observed fraction is not a
packet-loss rate. Final record is truncated again. Beacon clock offsets also
show more than one epoch (consistent with session restart); do not apply a
single median correction blindly. The local radio capture is only 242 KB,
while its IP companion is 42 MB, limiting dual-radio clock matching during
the full playback interval. This run establishes user-observed playback and
active input state, not yet independently verified media timestamp age for
the whole session.

Preserved current executable plus touched media/transport source and clock
tests in `checkpoints/ap-tsf-playback-20260904.tar.gz` (no pairing credentials
or captures). No performance or protocol changes made after this result.
Keep using `DRCD_AP_TSF_CLOCK=1`. Next investigate video throughput, repeated
IDR requests and radio retransmissions while preserving this clock source.

## Ready: opt-in actual AP TSF clock

Hardware probe confirms bank 0 port 0 advances and is about 154,267–154,326 us
behind received-packet free_run_cnt in this session. Other bank-0 ports stay
at 1; bank 1 returns `0xdeadbeefdeadbeef` (invalid register reads). This is
distinct from the prior Mac session's ~152630-us offset: never hardcode it.

`DRCD_AP_TSF_CLOCK=1` now selects actual RTL8852BE bank-0 port-0 TSF register
reads, sampled every 100 ms with monotonic extrapolation between readings.
High/low/high reads avoid torn rollover values; initial samples must advance.
Invalid, slow, stale or reset samples disable media rather than switching
clock domains. Log identifies the selected source and sampling failures.
Register reads use the shared debugfs read selector; do NOT run the probe
or another register debugger concurrently. No hardware register values are
written. No pairing, encoding, packetization, video/PCM scheduling or keepalive
changes. Disabled by default, preserving the prior baseline.

```sh
sudo env DRCD_AP_TSF_CLOCK=1 ./scripts/capture-drcd-session.sh /tmp/drcd-ap-tsf.pcap --np --play ./test1.mkv
```

Hardware playback with this source is pending. Tests cover clock progression,
read failure/recovery, inactive/frozen ports, reset rejection, stale samples
and 32-bit timestamp wrap. Start the Mac sniffer before connecting the GamePad
if possible to independently validate packet timestamp alignment to beacons.

## Driver clock source identified; hardware register probe pending

Installed driver: rtw89_8852be, kernel 7.2.2-1-cachyos. Upstream rtw89
`core.c` assigns RX `mactime` from `desc_info->free_run_cnt`, whereas
`rtw89_mac_port_get_tsf()` reads per-port TSFTR low/high registers. This
supports the independent capture's measured distinction between reported RX
counter and advertised AP clock. Stock upstream mac80211 ops do not expose
rtw89 get_tsf, and the standard debugfs TSF file is not added for AP mode.
The exact installed driver behavior still needs direct register confirmation.

Added `scripts/probe-rtw89-clock.py` for RTL8852BE only. With drcd and its
monitor running, it compares fresh GamePad RX timestamps to both banks' five
port TSF clocks. It changes only the debugfs `read_reg` selector, never hardware
registers, pairing or interfaces. Do not use another register debugger at the
same time. Syntax/help checked; privileged hardware execution is pending.

```sh
sudo python scripts/probe-rtw89-clock.py wlan0 | tee /tmp/drcd-port-clock.txt
```

Run in a second terminal during the normal baseline capture. Streaming source
and binary were not changed for this investigation. Clock corrections must
come from measured port state, not a hardcoded 152630-us session offset.

Sources: upstream Linux `drivers/net/wireless/realtek/rtw89/core.c`,
`mac.c`, `mac80211.c`, `reg.h`, `debug.c`; `net/mac80211/debugfs_netdev.c`.

## Independent Mac capture: measured clock-domain discrepancy

`mac_capture.pcap` plus simultaneous `/tmp/drcd-mac-observed*` now expose
176 PC AP beacons. Matching 2,304 received GamePad frames between the two
radios aligns their TSF clocks: advertised PC beacon TSF is approximately
152,630 us behind local reported radio TSFT. Media tracks the latter, so
its apparent near-zero local PCM age hides a large lead relative to beacon
time. This is a measured mismatch, not yet a proven complete root cause;
do not hardcode the observed offset as session-independent.

The Mac also sees 5,590 retry-bit-set observations among 14,764 AP data
observations, and local TX reports arrive well after matched on-air frames.
These are not packet-loss percentages. Original Mac capture has a truncated
last record; a separate complete-record prefix was analyzed. PC key access
requires user sudo authentication; no keys were displayed. Details:
`../drc-eeprom-exporter/MAC_CAPTURE_FINDINGS.md`. Streaming code unchanged.

## Current build: restored picture/audio-producing baseline

Read-only clock investigation completed: see
`../drc-eeprom-exporter/CLOCK_FINDINGS.md` and `analyze_clock.py`.
Brief-success and failed retries have nearly identical median local clock
behavior; no clock jump explains the successful transition. Real-console
beacons calibrate its AP clock, but local captures have no target AP beacons,
so local monitor-to-AP alignment and on-air delivery remain unproven. The
video/audio relative timestamp relationship differs by approximately 20 ms
between reference and host; this is not yet a demonstrated failure cause.
No streaming code changed during this investigation.

At user request, removed the optional media-clock experiment entirely.
The ACKed-keepalive refresh experiment was already reverted. No new
compatibility changes are included. Restored the media and keepalive behavior
from `/tmp/drcd-pcm-reference-ip.pcap`, where the user saw picture and heard
audio before corruption/freezing. That result still needs repeatability testing.
The old `DRC_REFERENCE_MEDIA_CLOCK` environment variable no longer has any effect.

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-pcm-baseline-retry.pcap --np --play ./test1.mkv
```

Sections below describe historical experiments, not additional current changes.

## Keepalive experiment REVERTED; optional clock test

User reported no picture/audio in `/tmp/drcd-keepalive-recovery-ip.pcap`.
Reverted the ACKed-query refresh branch and its test. Restored the keepalive
policy used in the picture/audio run. The failed run had no refresh-branch
log messages, so that branch was not demonstrated to cause the regression.

Different candidate: relative to nearby radiotap TSF samples, our video
timestamps lag by ~16–27 ms, whereas PCM is near zero. For the real console,
376 beacons establish AP TSF minus monitor TSF at median -5,176,998 us
(range -5,177,022 to -5,176,973). Correcting that separate monitor-clock offset
gives real video age about 6.24 ms and PCM about 9.78 ms. These estimates
include capture/transmission timing effects; they are not an exact deadline
specification and do not prove the freeze cause.

Default behavior is the restored picture-producing baseline. Opt-in
`DRC_REFERENCE_MEDIA_CLOCK=1` stamps video just before send with TSF-6250us,
keeps the format timestamp identical, and stamps PCM with TSF-10000us.
No changes to frame encoding, packet sizes, clock-field values or keepalive.
The runtime log explicitly identifies baseline versus experimental mode.

```sh
sudo env DRC_REFERENCE_MEDIA_CLOCK=1 ./scripts/capture-drcd-session.sh /tmp/drcd-clock-reference.pcap --np --play ./test1.mkv
```

Omit the environment variable to run the restored baseline. Hardware outcome
for the optional timing profile remains unverified.

## BREAKTHROUGH: physical video/audio; recovery test ready

User observed an image and heard audio with the reference PCM build, followed
by corruption and frozen A/V. `/tmp/drcd-pcm-reference-ip.pcap` confirms
waiting→active at 10.382 s, 1,745 complete outgoing frames, and PCM median
spacing 8,666 us. Preserve this media configuration. The successful transition
followed the PCM change, but does not establish which preceding changes are
also required. The active input flag does not guarantee ongoing playback.

Keepalive seq 11 completed after retries at 12.379 s. Seq 12 received ACKs but
never a reply; the next completed query was seq 13 at 24.422 s. Continuous
host media and repeated IDR requests persisted. This does not establish why
the picture corrupted or why audio froze; timestamp delivery, radio loss,
decoder recovery and frame cadence remain candidates.

The next build refreshes an ACKed UVC/UAC query with a new sequence after
one response timeout instead of retrying that same acknowledged transaction
for ten seconds. Unacknowledged queries retain the existing retry policy.
Late replies are still ACKed but cannot complete a different transaction.
Media encoding, headers, PCM size and schedule are unchanged. Added a
loopback regression test that ACKs a query, withholds its reply and checks
that the next interval issues a fresh sequence.

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-keepalive-recovery.pcap --np --play ./test1.mkv
```

## Previous hardware test: reference PCM size and cadence

`/tmp/drcd-format-reference-ip.pcap` confirmed the reference format bytes in
all 630 format messages, but all 2,383 input reports remained waiting and
508 IDR requests were captured. All 630 outgoing frames were complete.

The next binary changes only PCM packetization: 416 stereo s16 frames
(1664 payload bytes, 1672 including header) per packet at 48 kHz, scheduled
every 8,666,666 ns rather than 384 frames every 8 ms. Regression checks cover
the exact packet size, length bytes `06 80`, and the scheduler period.
Video, format packets and keepalive behavior remain as in the previous run.

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-pcm-reference.pcap --np --play ./test1.mkv
```

Hardware acceptance pending. This is a compatibility test, not a confirmed fix.

## Previous hardware test: reference video-format packet

Byte-0 test `/tmp/drcd-uvc-byte0-ip.pcap` failed to enable streaming: all
3,170 input reports stayed waiting, with 747 IDR requests and 868 complete
outgoing frames. Byte 0 did transition correctly, and keepalive replies
completed through sequence 18.

The current build additionally matches the authenticated real console's
video-format outer bytes `0400000011118108` and clock fields
16000/16000/21000/21000. The per-frame timestamp remains live, not replayed.
PCM, VSTRM encoding/options, pacing, and keepalive behavior are unchanged
from the previous test. Exact 32-byte format output is regression checked.
The capture analyzer now accepts the reference zero-length declaration with
its 24-byte format body, without suppressing other length mismatches.

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-format-reference.pcap --np --play ./test1.mkv
```

Hardware acceptance is pending; this is a targeted compatibility experiment.

## Previous hardware test: UVC/UAC byte 0 transition

The byte-order-only run `/tmp/drcd-uvc-le-ip.pcap` confirmed `80 3e` rate
bytes and `00 16` / `00 19` volumes, but all 2,846 input reports still had
the waiting flag set. It contained 886 complete outgoing frames at 50.64 fps.
Keepalive replies completed through sequence 15; sequence 16 retried without
a completed reply before shutdown. IDR requests exceeded 700.

The next binary additionally changes request payload byte 0 to 1 after a
validated UVC/UAC reply, matching the real-console sequence. First request
remains zero. Its meaning and effect on video remain unconfirmed. Regression
tests check both the initial and subsequent byte values. No media settings,
camera defaults, or command cadence changed in this experiment.

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-uvc-byte0.pcap --np --play ./test1.mkv
```

## Previous hardware test: UVC/UAC byte order only

The current binary now writes the 16000 Hz microphone rate little-endian
(`80 3e`) and mirrors both reply volume fields into subsequent requests in
little-endian order. This matches the authenticated real-console capture.
Request byte 0, camera defaults, command cadence, video format, PCM size and
video scheduling are unchanged. The loopback regression test checks the first
request rate and the synchronized fields on a later keepalive.

Run from `drc-project` (stop any previous drcd run first):

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-uvc-le.pcap --np --play ./test1.mkv
```

Check whether the GamePad leaves the Nintendo logo/error and whether input
state changes from waiting to active. Capture both success and failure; this
is a controlled compatibility experiment, not a confirmed fix.

## Real-console capture decrypted successfully

The separate `../drc-eeprom-exporter` Aroma v0.2 app exported a valid 768-byte
GamePad EEPROM. Its key verified both captured handshakes in
`/tmp/wiiu-real-165.pcap` using Nintendo's rotated PTK. The new offline decoder
authenticated 25,358 CCMP packets and excluded 383 authentication failures.
Private outputs: `/tmp/wiiu-real-decrypted.pcap` and
`/tmp/wiiu-real-decrypted-ip.pcap`. These contain sensitive control data; do not
publish them. Keys were not displayed.

First concrete reference differences: real video-format outer header
`0400000011118108` versus ours `0400001800001000`; real clock fields
16000/16000/21000/21000 versus four 16000s; real PCM 416 stereo samples per
packet versus our 384; video option ordering differs. These are candidates,
not a proven root cause. The analyzer incorrectly flags real format packets'
zero declared length as errors. The latest host cadence capture still measured
49.02 fps. See `../drc-eeprom-exporter/REFERENCE_FINDINGS.md` for details.
No streaming behavior was changed during this decryption checkpoint.

Startup analysis now identifies a stronger controlled-test candidate: real
UVC/UAC rate bytes at request offsets 12–13 are `80 3e`, versus our `3e 80`;
real subsequent volume fields are `00 16` / `00 19`, versus ours `16 00` /
`19 00`. Both sides send keepalive queries about once per second, so the
payload—not merely repetition—needs attention. Real request byte 0 changes
to 1 after the first query; ours stays zero. Do not blindly copy unknown
camera/default fields or replay captured generic commands that may write
settings. Real input transitions waiting→active at 15.9359 s after a reconnect;
our capture never becomes active. The next recommended isolated test is the
known UVC/UAC 16-bit byte-order correction, including reply synchronization.
This is a wire mismatch, not yet a proven video blocker. Analysis only so far.

## Latest diagnostic checkpoint (supersedes earlier radio conclusions)

September 4, 18:11 full media capture `/tmp/drcd-av-clock-ip.pcap`:
593 complete five-chunk video frames, no outbound sequence discontinuities,
no video timestamp regressions, and PCM median spacing exactly 8 ms. Input
and keepalive remained healthy, but the GamePad never left `waiting`.
The capture exposes a regression introduced by the 2 ms per-video-packet
pacing experiment: actual output was only 32.486 fps despite advertising
59.94 Hz. Full frame packet counts vary, so per-packet sleeps cannot preserve
the advertised frame cadence.

The current build removes that experiment and follows the reference frame
schedule: sample TSF before encoding, wait for the next frame boundary, then
send the format packet and the complete video frame together. The checker now
reports observed frame rate. A six-second loopback test using test1.mkv and an
IDR request for every frame produced 357 frames at 59.913 fps and 746 PCM
packets. This verifies host throughput only; physical GamePad acceptance is
still outstanding. Next hardware command:

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-av-cadence.pcap --np --play ./test1.mkv
```

The earlier hardware run, `/tmp/drcd-black-libdrc.pcap` (September 3, 23:29),
also failed with the original 59.94 Hz header combination. Keepalives reached
sequence 10 without timeouts; IDR requests exceeded 500. Display and audio
acceptance remain unverified.

Earlier claims that the adapter ignores RTS or necessarily aggregates six
packets per exchange were too strong. Same-adapter TX capture timestamps can
reflect reporting/buffering, ordinary ACKs were omitted from the original
ratio, and absence of locally transmitted control frames in a monitor capture
does not prove their absence on air. Neither a defective adapter nor a required
RTS pattern has been demonstrated. Do not recommend replacement based on those
counts alone.

The September 4 build fixes a concrete TSF sample race: the hardware timestamp
and its host sampling time are now read/written together under a mutex. It also
prevents PCM catch-up bursts after FFmpeg startup or a blocked read. Neither fix
has yet been tested on the physical GamePad. Monitor-derived TSF accuracy and
its relationship to the AP beacon clock still need measurement.

The capture wrapper now starts a simultaneous decrypted UDP capture before
launching drcd. Run the full audio/video path:

```sh
sudo ./scripts/capture-drcd-session.sh /tmp/drcd-av-clock.pcap --np --play ./test1.mkv
```

It produces the radio capture plus `/tmp/drcd-av-clock-ip.pcap`. After stopping:

```sh
python3 scripts/analyze-media-capture.py /tmp/drcd-av-clock-ip.pcap
```

The checker reports complete five-chunk frames, sequence discontinuities,
timestamp regressions, payload length errors, and host video/PCM spacing.
It supports tcpdump Ethernet, Linux cooked v1/v2, and raw IPv4 classic pcaps.
A host capture validates transmission, not reception or decoding. Build and
all three CTest tests passed; synthetic checker cases covered sequence and
timestamp wraparound, missing packets, complete frames, and cadence.

This is the authoritative status and investigation handoff for `drc-project`.
The immediate goal is to make a real Wii U GamePad work with `drcd` by itself.
Cemu integration is intentionally deferred until the standalone network,
control, input, audio, and video paths are reliable.

## Current verdict

The GamePad is paired and the bidirectional runtime network is operational.
It authenticates, obtains its expected address, answers command requests, and
sends valid controller input continuously. The latest hardware run still
failed the media session: the GamePad showed the Nintendo logo for about ten
seconds and then reported that it could not connect to the Wii U. That run
included the corrected delayed x264 chunk copy, strict command transactions,
recurring keepalive, and a coherent current-Strawberry 50 Hz media format.
Video and audio therefore cannot yet be called working.

The on-screen error must not be treated as proof that Wi-Fi disconnected.
During the same state, `drcd` continues receiving GamePad HID input. Current
evidence now points first to Wi-Fi media delivery. A complete real-console
capture shows that the Wii U places almost all media in QoS TID 5 and uses
RTS/CTS for nearly every such frame, whereas the failed `drcd` capture placed
almost all outgoing data in TID 0 and sent no RTS frames. `drcd` now marks its
video/audio sockets for TID 5. The RTL8852BE rejected per-TID RTS configuration
but accepted a global RTS threshold during the next hardware retest; behavior
was unchanged and the setting restored cleanly on stop. The current build also
requests the real console's observed HT20 MCS 6 / 58.5 Mbit/s transmit rate,
which still needs a GamePad run. The recurring UVC/UAC keepalive is
hardware-verified. Basic association, DHCP, routing, input, and keepalive are
no longer leading suspects.

| Layer | Status | Evidence |
|---|---|---|
| Patched 5 GHz AP | Working | The RTL8852BE creates the Wii U-style AP and the GamePad associates. |
| WPS pairing | Working | Pairing completes and produces reusable runtime credentials. |
| Runtime WPA2 | Working | `AP-STA-CONNECTED` and `EAPOL-4WAY-HS-COMPLETED` are logged. |
| IPv4/DHCP | Working | The GamePad accepts `192.168.1.11`; the host is `192.168.1.10/24`. |
| Command transport | Working and hardware-verified | UIC and UVC/UAC replies are matched and ACKed; the one-second keepalive continued for the full latest run with no timeouts. |
| GamePad input | Working | Valid 128-byte reports arrive around 180 Hz on UDP 50022. Buttons and sticks decode correctly. |
| Hardware TSF source | Implemented and observed | `drcdtsf` supplies radiotap TSFT and the media clock reports synchronization. |
| Wi-Fi media QoS | Hardware/driver remains possible but unproven | Global RTS was accepted without changing the failure; the current build additionally fixes the runtime TX rate to the real console's HT20 MCS 6. |
| Audio delivery | Packet layout verified; hardware unconfirmed | PCM packet size, flags, timestamp endianness, and 8 ms pacing match maintained/reference implementations. |
| Video delivery | Encoder fixed; framing/timing investigation continues | The corrected IDR decodes locally and matches original x264 output byte-for-byte, but the latest GamePad run still requested continuous IDRs. |
| Cemu integration | Deferred | Work should remain in standalone `drc-project` until media works. |

## Tested hardware and identity

- Host kernel: `7.2.2-1-cachyos`.
- Wi-Fi chipset: Realtek RTL8852BE, PCI ID `10EC:B852`.
- Linux driver: `rtw89_8852be`.
- Wi-Fi interface: `wlan0`; normal internet access is over Ethernet.
- Emulated Wii U/AP MAC: `40:d2:8a:bf:fc:a8`.
- GamePad MAC: `40:d2:8a:ab:90:00`.
- Proven working channel for the current pairing/runtime: channel 149,
  5745 MHz. The GamePad's search channel can change after a power cycle.
- Pair code: `2220`, shown as `♦ ♦ ♦ ♠`.
- Console-side address: `192.168.1.10`.
- GamePad address: `192.168.1.11`.
- Interface MTU: 1800.

The AP MAC matters because the GamePad sends directed probes derived from the
console identity. Random or locally administered test MACs produced visible
networks but did not reproduce the successful discovery path.

## Proven working behavior

### Pairing and AP lifecycle

`drcd` owns the complete Linux AP lifecycle:

- temporarily releases `wlan0` from NetworkManager;
- normalizes the interface type and MAC;
- runs the repository's patched `hostapd`;
- generates Wii U-like pairing and runtime configurations;
- arms the WPS PIN through the hostapd control socket;
- stores successful credentials in `/var/lib/drcd/credentials.conf`;
- transitions automatically from pairing to the secured runtime AP;
- waits 60 seconds after pairing before re-entering the pairing cycle;
- restores the interface and terminates cleanly on Ctrl-C.

Automatic mode checks for a saved GamePad for five seconds, enters pairing for
twenty seconds, and repeats. `--np` disables the pairing cycle and offers only
the saved runtime network.

### Runtime network

The current log proves all of the following in order:

1. The GamePad associates as `40:d2:8a:ab:90:00`.
2. The custom/Tendonin-compatible RSN four-way handshake completes.
3. The station is authorized by hostapd.
4. The built-in DHCP server sends an OFFER and ACK.
5. The GamePad starts sending application-protocol traffic.
6. The command channel sends an ACK followed by a full UIC reply.

This rules out the saved PSK, basic RSN selector translation, DHCP address,
subnet, and UDP input route as causes of the current display failure.

### Input

The transport accepts the GamePad's 128-byte reports and decodes the format
used by the reference `libdrc` implementation:

- button bytes 2-3 plus extended-button byte 80;
- four little-endian stick values at bytes 6-13;
- big-endian sequence counter at bytes 0-1;
- battery and audio-volume status fields.

Observed working controls include A, B, X, Y, all D-pad directions, L, R, ZL,
ZR, Plus, Minus, Home, and TV. Both sticks reached approximately their expected
raw extremes (about 900-3200) and returned to centers near 2000. Button changes
are logged immediately; stick-only changes are throttled to eight lines per
second. A packet-log comparison and loopback test confirmed that the sequence
counter is big-endian; the earlier little-endian interpretation was incorrect.

This is especially important evidence: the GamePad-to-host application path is
healthy even while the GamePad screen displays the connection failure.

### TSF/media clock

The RTL8852BE driver does not expose the old patched-libdrc sysfs TSF files:

```text
/sys/class/net/wlan0/device/tsf
/sys/class/net/wlan0/tsf
```

The backend now creates a `drcdtsf` monitor interface on the same PHY while the
runtime AP is active. `RuntimeTransport` reads radiotap TSFT samples from that
interface and extrapolates between samples. Logs have confirmed synchronization
to a real hardware TSF value. The old transport-relative clock remains only as
a fallback.

Video still failed after hardware TSF synchronization, so complete TSF absence
is ruled out. Timestamp epoch, sampling semantics, truncation, and alignment
with frame transmission still need direct comparison.

## Current media implementation and failure

Run the standalone media test with:

```sh
sudo ./build/drcd/drcd --np --play ./test1.mkv
```

`test1.mkv` is a 23 MiB local test asset with audio and video. The current path:

1. FFmpeg loops and scales video to the GamePad's 864x480 H.264 surface at
   60000/1001 Hz, YUV420P.
2. The repository's patched `drc-x264` encodes DRH-mode macroblock chunks.
3. `drcd` wraps chunks in 16-byte VSTRM headers and sends them to UDP 50120.
4. A video-format ASTRM packet is sent to UDP 50121 before each video frame.
5. Stereo 48 kHz signed 16-bit PCM packets are also sent to UDP 50121.
6. Timestamps come from the hardware TSF clock when available.

The VSTRM and ASTRM header layouts were checked against the adjacent reference
`libdrc` implementation. UDP sends report no socket errors. Nevertheless:

- the GamePad remains on the Nintendo logo for roughly ten seconds;
- it then displays `Could not connect to the Wii U console`;
- no test frame has appeared;
- it sends repeated four-byte `01 00 00 00` messages to UDP 50010, which are
  IDR/resynchronization requests;
- one failed run produced roughly 913 IDR requests in about ten seconds while
  command and UDP sends remained clean.

Repeated IDR requests are the strongest current signal. They mean that the
GamePad is alive and explicitly rejecting, failing to assemble, or failing to
decode the supplied video stream.

## Fixes implemented after the last hardware run

The media investigation reproduced a corrupted IDR without involving Wi-Fi.
The patched x264 callback reports CABAC ranges before carry propagation has
necessarily finalized every byte. `MediaStreamer` copied each range inside the
callback, so bytes at DRH chunk boundaries could be stale. It now retains the
x264 pointers and lengths and copies all five chunks only after the complete
frame has finished encoding, matching reference `libdrc`. A reconstructed IDR
now decodes with FFmpeg without errors.

The same change set also:

- requires exactly five chunks and places them by `i_first_mb`;
- rejects unexpected, duplicate, missing, or non-IDR callback results;
- emits the original libdrc video-format ASTRM clock layout;
- writes the first valid IDR, its raw chunks, video-format packet, VSTRM
  datagrams, and a manifest to `/tmp/drcd-media-dump/` on every media run;
- decodes and logs transitions of HID byte 83 bit 0;
- tracks UIC and UVC/UAC commands by query type and sequence, validates replies,
  ACKs them, logs latency/retries, mirrors returned microphone/camera state, and
  sends UVC/UAC again one second after each valid response;
- delays media startup until both GamePad input and a valid UVC/UAC response
  have arrived;
- stops and reinitializes media after a protocol or hostapd disconnect.

Local verification completed successfully:

```text
cmake --build build --target drcd drcd_core_tests drc_host_tests --parallel 4
ctest --test-dir build --output-on-failure
ffmpeg -v error -i /tmp/drcd-media-self-test.h264 -frames:v 1 -f null -
```

The loopback transport test covers initial UIC/UVC exchange, distinct command
sequence IDs, UVC/UAC keepalive repetition, reply ACKs, HID sequence endianness,
the waiting-for-streaming flag, and media-ready gating.

### Latest hardware run after the encoder fix

The 2026-09-03 22:24 run proved that the command fix works on the real GamePad:
UIC and UVC/UAC both completed, subsequent UVC/UAC transactions completed every
second in 0-8 ms, and there were no command timeouts or UDP send errors. Media
started only after input plus UVC/UAC readiness as intended.

Video was still rejected: HID remained `waiting`, the GamePad sent 500 IDR
requests, and no visible stream was established. The captured first IDR had five
correct chunks and decoded cleanly. A separately built original pc2drc x264
produced identical chunk sizes and SHA-256 hashes for the same black frame, which
rules out the modern x264 port and payload bytes for that frame.

That run used the maintained Strawberry project's experimental ASTRM layout.
A following hardware run restored the original libdrc media clock fields but
still reached 700 IDR requests with HID remaining `waiting`. A third run also
applied pc2drc's outer ASTRM timestamp of zero and reached 800 requests without
changing the result. The current build now uses Strawberry's complete coherent
wire/timing combination rather than a hybrid: 50 Hz encoding and pacing, the
50 Hz VSTRM option ordering, `video_format=1`, disabled video clocks, 16 kHz
synchronization clocks, and outer timestamp `0x00100000`.

## Implementation map

- `drcd/src/backend_linux.cpp`
  - interface preparation and restoration;
  - pairing/runtime hostapd configuration;
  - automatic pairing loop and channel handling;
  - DHCP server;
  - `drcdtsf` monitor lifecycle;
  - runtime transport/media startup.
- `libdrc-host/src/runtime_transport.cpp`
  - UDP ports 50010, 50020-50023, and 50025;
  - command query/reply ACK handling;
  - IDR request detection;
  - HID input decoding and change-only diagnostics;
  - TSF sysfs/monitor/fallback clock sources;
  - outbound UDP statistics.
- `drcd/src/media_streamer.cpp`
  - FFmpeg input conversion;
  - patched x264 setup;
  - VSTRM and ASTRM packet creation;
  - frame/audio pacing and resync handling.
- `third_party/hostapd-drc/`
  - pinned hostapd build and Tendonin/Wii U compatibility patches.
- `third_party/drc-x264/`
  - DRH-capable x264 fork used by the standalone test streamer.
- `scripts/watch-gamepad-probes.sh`
  - passive channel watcher for directed GamePad probes.

Useful adjacent references, not part of this repository, are `../libdrc/`,
`../vanilla/`, and `../drc-wireshark-plugins/`.

## Investigation plan

The next work should change one variable at a time and preserve a packet-level
artifact for every run.

### Priority 0: prove the exact outgoing media bytes (completed locally)

1. Add a bounded diagnostic dump of:
   - the video-format ASTRM packet;
   - the first complete IDR frame's VSTRM packets;
   - each x264 callback's NAL type, reference priority, `i_first_mb`, and size;
   - per-frame chunk count and VSTRM flags.
2. Assert the invariants expected by reference `libdrc`:
   - exactly five ordered DRH macroblock chunks per frame;
   - chunk indices derived from `i_first_mb`, rather than merely callback order;
   - no SPS, PPS, AUD, or SEI data in the DRH payload;
   - sequence IDs wrap at 1024;
   - init is set only on the first frame;
   - frame-begin/chunk-end/frame-end flags match chunk boundaries.
3. Compare the bundled `drc-x264` patch and output byte-for-byte with the x264
   revision expected by `../libdrc`. Matching encoder settings alone is not
   enough; the GamePad depends on nonstandard raw macroblock-row output.
4. Reconstruct the dumped chunks into a conventional H.264 stream using the
   known GamePad SPS/PPS and synthetic slice headers. Decode it locally to
   distinguish invalid encoder output from invalid VSTRM wrapping.

This branch found and fixed the stale-CABAC-byte corruption. The hardware run
must now confirm that the GamePad accepts the corrected output.

### Priority 1: observe GamePad streaming state and timing

1. Decode and log HID byte 83 bit 0, identified by the existing Wireshark
   dissector as `Waiting for streaming`. Log transitions only.
2. Correlate that bit with:
   - association and DHCP time;
   - first command reply;
   - first ASTRM/VSTRM packet;
   - first and subsequent IDR requests;
   - the on-screen timeout.
3. Record, for each first-frame packet, hardware TSF, VSTRM timestamp, send
   time, sequence, and payload size.
4. Verify whether the GamePad expects the ASTRM video-format packet before the
   first VSTRM frame only, once per frame as reference `libdrc` does, or with a
   specific lead time.
5. Test a fixed black frame at an exact 59.94 Hz before involving FFmpeg audio
   or file-loop timing.

### Priority 2: make command/keepalive handling observable and strict (implemented)

The command implementation now has explicit outstanding transactions, strict
reply validation, bounded retries, retained response payloads, and detailed
transition logging. Its startup and one-second UVC/UAC keepalive behavior have
loopback coverage; the real GamePad still needs to verify it.

1. Log one decoded line per command transition: packet type, query type,
   sequence, payload length, retry, and reply latency.
2. Track outstanding requests by sequence instead of issuing independent
   periodic queries without an explicit state object.
3. Validate and retain the full UIC and UVC/UAC response payloads.
4. Compare startup ordering and retry behavior with `../libdrc`'s `CmdClient`
   and `UvcUacStateSynchronizer`.
5. Check whether the real Wii U sends time query type 2 or another startup
   command that the reference host library omits.
6. Only investigate UDP 50025/50125 after the known command and streaming
   state is instrumented; reference `libdrc` does not require that port for its
   basic streamer.

### Priority 3: controlled experiment matrix

Run and record these separately:

| Experiment | Purpose |
|---|---|
| `--np`, no media | Baseline lifetime of WPA, DHCP, commands, input, and HID byte 83. |
| `--np --play ./test1.mkv` | Reproduce current failure with bounded media dumps. |
| Static black IDR frames, no audio | Remove FFmpeg content and audio concurrency. |
| Video-format ASTRM only | Observe whether HID streaming state changes without VSTRM. |
| VSTRM with fixed hardware timestamp | Separate timestamp progression from payload validity. |
| Normal TSF versus fallback clock | Determine whether behavior changes, not merely whether TSF exists. |
| Audio only | Establish whether ASTRM audio reaches the GamePad independently. |

Do not return to channel sweeping, pairing IE work, or DHCP changes unless a
new capture shows an actual regression in those already-proven layers.

## Captures, logs, and commands

The daemon's comprehensive log is replaced at startup and then retained across
all phases of that run:

```text
/tmp/drcd.log
```

The current unfiltered monitor capture is:

```text
/tmp/drcd-onair.pcap    approximately 18 MiB / 36,649 captured packets
```

It came from a failed display attempt. Confirm its channel and relevant MAC
coverage before treating it as a golden capture. Runtime 802.11 data is
encrypted on air, so a simultaneous host-side IP capture is more immediately
useful for validating `drcd` packets:

```sh
sudo tcpdump -i wlan0 -U -s 0 \
  -w /tmp/drcd-runtime-ip.pcap \
  'udp and (host 192.168.1.10 or host 192.168.1.11)'
```

The latest hardware run used a coherent current-Strawberry media combination:
50 Hz video, Strawberry VSTRM extension order, and Strawberry video-format
clocks/format byte. Association, DHCP, UIC, and recurring UVC/UAC replies were
healthy with zero command timeouts, but the pad remained `waiting` and sent
more than 500 IDR-resync requests before disconnecting. This makes Wi-Fi
delivery/startup timing a higher-priority discriminator than further blind
encoder-format changes.

Capture a real Wii U session with radiotap and all 802.11 ACK/BlockAck frames:

```sh
sudo ./scripts/capture-wiiu-session.sh wlan0 40:d2:8a:ab:90:00 auto
```

Auto mode reports the detected channel. Stop it, then rerun using that fixed
channel before powering on the GamePad to retain association/EAPOL and the
complete media startup. The real-console runtime payload is encrypted without
that console's runtime key, but frame direction, QoS, retry/ACK behavior,
length, rate, and timing remain directly comparable.

The capture script must use its dedicated `drcsniff` monitor child. On the
RTL8852BE, changing the base `wlan0` interface itself to monitor mode produced
Ethernet frames inside a pcap advertised as radiotap; consequently no 802.11
MAC filter could match. The first attempted fixed-channel capture at
`/tmp/wiiu-real.pcap` has that malformed link-layer combination and contains
zero GamePad frames, so it is not usable as a reference capture.

The subsequent hopping capture `/tmp/wiiu-scan-raw.pcap` is valid and located
the live real-console session on channel 165 (5825 MHz). It confirms the real
console/BSSID is `40:d2:8a:bf:fc:a8` and the GamePad is
`40:d2:8a:ab:90:00`, exactly matching the identities used by `drcd`. In the
brief channel-165 window it captured 292 console-to-pad data frames, 109
pad-to-console data frames, and corresponding RTS/CTS/BlockAck exchanges. The
real console transmitted QoS data at MCS 6 / 58.5 Mbit/s; the pad transmitted
at MCS 5 / 52 Mbit/s.

The completed fixed-channel capture is:

```text
/tmp/wiiu-real-165.pcap    98,535 packets / approximately 23 MiB / 45 seconds
```

It contains two complete real-console associations and WPA handshakes followed
by sustained encrypted runtime traffic. Directional comparison against the
failed `/tmp/drcd-onair.pcap` found:

| Capture | AP-to-pad data | QoS TID distribution | AP-originated RTS |
|---|---:|---|---:|
| Real Wii U | 18,162 | TID 5: 17,356; TID 1: 781; TID 0: 16 | 17,013 |
| Failed `drcd` run | 13,799 | TID 0: 13,752; TID 7: 5 | 0 |

The real console's steady AP media rate was MCS 6 / 58.5 Mbit/s and its retry
bit was set on only 45 of 18,162 AP-to-pad data frames. Both captures include
BlockAck negotiation, so the actionable difference is traffic classification
and RTS/CTS rather than absence of aggregation setup. The current build applies
`SO_PRIORITY=5` plus IPv4 TOS `0xa0` to video/audio sockets. The first hardware
retest accepted those socket settings but otherwise behaved identically:
UVC/UAC keepalives reached sequence 12 with zero timeouts, the pad remained
`waiting`, and it sent 415 IDR resync requests. RTL8852BE rejected
`iw dev wlan0 set tidconf tids 0x20 rtscts on` with "unsupported TID
configuration", meaning that run did not test RTS/CTS. The current build now
tries `iw phy phy0 set rts 0` as a non-fatal fallback and restores `rts off`
when the session ends. The next hardware run confirmed both operations but had
the same failure: keepalives reached sequence 16 with zero timeouts, the pad
remained `waiting`, and it sent 603 IDR requests. Global RTS alone is therefore
not sufficient. Earlier `drcd` radiotap metadata reported outgoing traffic at
the 6 Mbit/s legacy rate while the real console holds HT20 MCS 6 / 58.5 Mbit/s;
the current build requests that fixed HT rate and restores automatic rate
selection on stop. Same-interface capture metadata cannot confirm whether the
firmware honors that rate for the actual over-air media transmission.

The completed `drcd` radiotap capture is:

```text
/tmp/drcd-qos-test.pcap    31,154 packets / approximately 13 MiB
```

It proves the socket classification change worked: 10,599 AP-to-pad QoS data
frames used TID 5, versus 34 on TID 0 and 3 on TID 7. The one-second UVC/UAC
keepalive remained healthy through sequence 15 with zero command timeouts, but
the GamePad remained `waiting` and issued more than 500 IDR requests. The
same-interface radiotap metadata still reported most locally transmitted
frames at 6 Mbit/s or without a rate field, even though the MCS 6 request was
accepted, so that metadata cannot prove the firmware's actual over-air rate.

The most important new difference is transmit pacing. Consecutive TID 5 frames
from `drcd` had a median gap of 1 microsecond, with 9,391 of 10,599 gaps below
100 microseconds. The real Wii U median was 1,997 microseconds, with only 355 of
17,356 gaps below 100 microseconds. The GamePad returned 1,674 BlockAck frames
to `drcd` for 10,599 TID 5 frames, compared with 16,988 BlockAck frames for
17,356 real-console TID 5 frames. Later control-frame counting showed that many
`drcd` packets receive ordinary ACKs, so the raw QoS-to-BlockAck ratio must not
be interpreted as an aggregate size. The reliable mismatch is that the real
console predominantly uses BlockAck and RTS while `drcd` predominantly
receives ordinary ACKs and emits no observable AP-originated RTS.

The first static-black, video-only run at `/tmp/drcd-black-paced.pcap` removed
file decoding and PCM audio but still failed: keepalives reached sequence 16
with zero retries or timeouts, while the pad remained `waiting` and sent 666
IDR requests. It contained 4,550 AP-to-pad QoS frames, 725 GamePad-to-AP
BlockAck frames, and zero AP-to-GamePad RTS frames. The approximately 6.3:1
QoS-to-BlockAck ratio was unchanged. Of 4,549 consecutive QoS gaps, 3,155 were
still below 100 microseconds and the median remained 1 microsecond.

That run exposed a flaw in the first userspace-pacing implementation: packet
deadlines were relative to the frame start, so repeated IDR encoding made
several deadlines expire before sending began. The current build anchors
pacing after encoding and enforces a two-millisecond interval between VSTRM
datagrams. It also prevents frame-clock catch-up bursts after an overrun.

The corrected `/tmp/drcd-black-paced2.pcap` still failed with healthy
keepalives through sequence 19. It contains 5,383 AP-to-pad QoS frames, 931
GamePad-to-AP BlockAck frames, and zero AP-to-GamePad RTS frames. The following
`/tmp/drcd-black-noagg.pcap` run proved that the driver accepts independent
per-TID `ampdu off` and `amsdu off` requests, but it also failed with continuing
IDR requests and zero AP-to-GamePad RTS. The aggregation experiment is removed
from the current build because it did not change GamePad media acceptance.

A byte-for-byte comparison then identified a complete untested media mode.
The previous fallback experiments changed individual original-libdrc fields
while leaving a hybrid 50 Hz mode. The current build now uses the full original
working libdrc combination together: 60000/1001 Hz encoding and pacing, VSTRM
framerate value zero, IDR option before the framerate option, all four 16 kHz
ASTRM video-format clock fields, and `video_format=0`. Capture that controlled
black-frame test using another new output name:

```sh
sudo ./scripts/capture-drcd-session.sh \
  /tmp/drcd-black-libdrc.pcap --np --black
```

The capture wrapper now starts both `drcd` and `tcpdump` in separate sessions.
On Ctrl-C it signals `drcd` directly, allowing it to restore RTS and bitrate
configuration before stopping hostapd and taking the interface down.

Useful concise log extraction:

```sh
rg 'AP-STA-CONNECTED|EAPOL-4WAY|dhcp:|runtime-qos:|runtime-protocol:|Gamepad' /tmp/drcd.log
```

Incremental build and tests:

```sh
cmake --build build --target drcd --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

Runtime-only input/control test:

```sh
sudo ./build/drcd/drcd --np
```

Runtime media reproduction:

```sh
sudo ./build/drcd/drcd --np --play ./test1.mkv
```

The credential file contains the runtime PSK and must not be posted publicly:

```text
/var/lib/drcd/credentials.conf
```

## Definition of the next milestone

The next milestone is not merely “UDP sends succeed.” It is reached when all of
the following are true in one standalone `drcd` run:

1. The GamePad associates and receives `192.168.1.11`.
2. UIC/UVC command exchange remains healthy.
3. HID byte 83 exits the waiting-for-streaming state.
4. IDR requests stop repeating continuously.
5. A known static test image is visibly displayed for at least 60 seconds.
6. Controller input continues during display.
7. The daemon exits cleanly and can repeat the result after a GamePad reboot.

Only after this milestone should the transport be exposed to Cemu as a stable
packet/media/input API.
