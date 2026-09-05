# Recovery-slot hardware result: 2026-09-05

User reports much smoother playback, top-screen artifacts essentially gone,
and substantially improved stability, with some remaining motion artifacts.
Preserve this live configuration; no further streaming changes were made during
this analysis. Inputs are `/tmp/drcd-recovery-slots-ip.pcap`, its `.log` and the
companion radio capture. Different scenes/durations limit before/after comparisons.

## Verified sender behavior

- About 125.22 seconds of captured video; 7,444 complete frames and one incomplete
  final shutdown frame. No captured video sequence discontinuities, inconsistent
  timestamps, or payload-length errors. Mean video rate: 59.45 frames/second.
- 35 complete IDRs followed by video; all 35 have a format-only gap and consecutive
  video packet sequences across it. No consecutive IDRs. Median IDR-to-next-video
  timestamp difference: 33,367us.
- 105 recovery request observations during video, plus 15 after video stopped.
  Do not report the latter as failures of frames still being streamed.
  The earlier research-sync run had 2,202 requests over about 74 seconds.
- All 7,444 complete frames (35 IDR + 7,409 P) decode with FFmpeg's strict error
  checks after reconstructing receiver headers. This is software decodability,
  not proof of faithful reconstruction relative to Cemu or successful pad decoding.
- Format spacing: median 16,683us, range 16,147–17,225us. PCM spacing: median
  8,667us, range 7,648–9,687us. Neither has sub-100us catch-up bursts.

## Motion/complexity evidence

Times below are relative to first captured video, not emulator launch.
Frame size is a complexity/load proxy, not a direct measurement of motion.

| Interval | Video frames | IDRs | Maximum encoded frame bytes | Recovery requests |
| --- | ---: | ---: | ---: | ---: |
| 5–10s | 283 | 4 | 59,178 | 15 |
| 10–15s | 300 | 0 | 5,167 | 0 |
| 15–20s | 299 | 0 | 14,604 | 0 |
| 20–25s | 290 | 9 | 9,352 | 22 |
| 75–80s | 300 | 0 | 4,126 | 0 |

The second logged five-second encode interval peaks at 63,574us; subsequent
recovery requests also occur with much faster encoding. The independent format
clock stays regular through the long encode. Slow encoding can produce video
hitches but does not, by itself, explain all remaining requests or corruption.
The longest observed complete video-send duration is about 13.05ms.

Sampled reconstructed frames show the WWHD introductory artwork, sky/title
sequence and file-selection screen. There is visible softness/block structure in
the sampled sky imagery, but without the original source frames this cannot be
assigned specifically to our encoder rather than the source/rendering. A clean
syntax decode likewise does not exclude visible quantization artifacts.

## Delivery evidence and remaining limit

The local radio capture contains 59,742 protected AP-to-pad QoS data observations,
but no outgoing retry flags. That is not evidence of zero physical retries: the
transmitting adapter's monitor path need not expose actual on-air retries.
Only two observations matched the conservative compressed Block ACK lookup;
all others have unknown receipt status, not demonstrated loss. Ordinary ACKs do
not provide the packet identity needed for this comparison.

Authenticated payload matching is pending: `/var/lib/drcd/credentials.conf` is
not readable in the analysis session. Do not change its permissions or expose
its contents. The existing local analyzer reads it without printing keys:

```sh
sudo python drc-eeprom-exporter/analyze_delivery.py \
  --radio /tmp/drcd-recovery-slots.pcap \
  --host /tmp/drcd-recovery-slots-ip.pcap
```

Run from the workspace root. Even after authentication, a local TX observation
is not pad receipt; independent Mac capture may still be needed for delivery.

## Visual separation of encoder and delivery

Created `/tmp/drcd-motion-review-XtP810/host-video.mp4`: the exact reconstructed
encoded video, remuxed without re-encoding, excluding the incomplete final frame.
Compare the same scene there with what appeared on the GamePad. This is video
only at a fixed 60000/1001 playback rate; omitted recovery/slow-encode slots are
not represented, so it runs slightly faster and is not a latency/audio test.
The six-image contact sheet `samples.png` is only for finding scenes, not for
pixel-level judgment (it is downscaled).

If the same artifact is visible in this replay, it exists before radio delivery;
source-versus-encoder quality then needs a simultaneous source reference. If it
only appears on the pad, focus on delivery/deadlines/receiver behavior. Neither
outcome should trigger a broad encoder rewrite without that comparison.
