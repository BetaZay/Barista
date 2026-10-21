# GamePad video experiment checklist

Checked entries have a recorded outcome, not necessarily a successful fix.
Detailed measurements are in [the experiment report](gamepad-recovery-experiment.md).

- [x] **1694-byte video payloads — retained.** Matches reference packetization;
  user reported a large reduction in lingering corruption.
- [x] **Kernel receive timestamps for monitor TSF — retained.** Corrects socket
  queue delay in the clock anchor; user reported much smoother video.
  Capture: `20261004-155328-a1vbpa18`.
- [x] **Fast encoder search, preset 9 — rejected.** User reported worse/more
  corruption. Preset 5 restored in source and installed engine. Fewer recovery
  requests did not predict the visual result.
  Capture: `20261004-161257-49gw41n8`.
- [x] **Real packet/re-encoded replay comparison — abandoned.** Physical replay
  did not work for the user, who requested moving on. Offline artifacts and
  scripts remain available; do not resume this path without a new request.
- [x] **Match real-console video option order — retained for continued testing; effect inconclusive.** IDR options
  `80 82 00 83 85 06 00 00`; P options `82 00 83 85 06 00 00 00`.
  Retains option values, encoder, timing, packet sizes and recovery behavior.
  `DRCD_REFERENCE_VIDEO_OPTIONS=0` restores the earlier order when explicitly
  configured in the engine environment. General parsers accept either order;
  a receiver-side effect is a hypothesis, not an established fault.
  Capture: `20261004-162913-oovfvghi`. The 90-second trial completed and the
  captured options match the reference, but recovery requests rose to 782
  (versus 300 in the earlier smoother run). Different interactive scenes
  prevent assigning that increase to option order. The experimental default
  remains installed. The user reports less corruption than the immediately
  preceding fast-search trial, with corruption around fades/slide-ins and
  slight jitter. Because preset 5 was restored too, this does not isolate an
  option-order benefit or establish improvement over the earlier smoother
  baseline. It is not accepted as a corruption fix.

- [x] **Keep chunk deadlines anchored to the format slot — retained provisionally for pacing.**
  Previously, a late first video packet shifted every later chunk deadline.
  In the option-order host capture, 35 of 4,594 complete matched frames began
  at least 1 ms late, with a maximum of 4,040 us. The change uses the originally
  committed frame deadline for all packet offsets, retaining all encoded
  frames and packet order. Overdue packets send immediately, so watch for
  compressed packet spacing as well as reduced completion delay. No previous
  observed second-chunk end exceeded the nominal restart budget; this is a
  jitter/delivery timing hypothesis, not established decoder starvation.
  Capture: `20261004-163540-0u5t24r3`. Later host chunk deadlines stayed closer
  to their intended schedule (last-chunk maximum 13,063 us versus 15,045 us
  previously). A connection retry shortened active video, and this trial
  contained fewer large compressed frames. The user reports about the same
  corruption and seemingly fewer lag spikes. The change remains installed
  provisionally as a pacing improvement, not a corruption fix. A comparable
  full-length active-video run would strengthen that assessment.

- [x] **250-ms interval for requested recovery IDRs — rejected.** A
  request arriving after the existing IDR/gap/first-P lifecycle remains
  pending until eligible. Startup and source-change IDRs still happen
  immediately. Repeated recovery requests can otherwise generate several
  expensive IDRs per second during transitions. The interval is experimental,
  not a known firmware requirement; it may delay legitimate second recovery.
  User reports both more lag and more corruption. Capture:
  `20261004-164420-vmxwn__x`. Gate removed from source and the previously
  tested engine restored. Do not repeat this cooldown without new evidence.
- [x] **Colored fade and sliding-graphic reconstruction — verified offline.**
  Added 48 synthetic frames; production bytes and every reconstructed pixel
  match the native encoder reference. Retained as deterministic coverage,
  not evidence that the physical decoder accepts every game transition.

- [x] **Packet-count-aware chunk spacing — no visual improvement; reverted.** Multi-packet
  chunks no longer always fill the entire available chunk window. Use 600-us
  gaps for small chunks, compressing them when needed to retain the original
  maximum completion deadline. Single-packet chunks and chunk start deadlines
  remain unchanged. The real capture's first multi-packet chunk completes at
  a median 642 us, versus 2,497 us in the preceding Barista capture. This
  compares different content and is a compatibility hypothesis, not a measured
  decoder starvation failure. Recovery cooldown remains removed.
  Capture: `20261004-165305-wlpq8_na`. Actual two-packet first chunks finish
  at a median 596 us, confirming the change. User reports about the same
  corruption and jitter. Restore the previous spacing rather than retain an
  unhelpful additional experiment; this result does not rule out other timing
  problems. Active video was 48.78 seconds after a connection retry.

- [x] **Reopen recovery requests at resumed P start — retained provisionally.**
  Clear settling IDR/gap requests immediately before sending the first
  resumed P instead of after its final packet. A request during that frame
  remains eligible for recovery; generation checks still protect a newer
  IDR from an older P sender. The last capture contains one radio request
  10,758 us into a resumed P window. This is limited evidence of a potentially
  discarded request, not proof of a decoder failure or its root cause.
  Packet scheduling, encoder and the IDR/format-only/P pattern stay unchanged.
  A local integration test injects a request 5 ms into a resumed P and checks
  that a recovery IDR follows within 100 ms, before the next periodic request.
  Capture: `20261004-170115-xk4fvqc0`, 88.55 seconds of active video. User
  reports a tentative improvement in both corruption and jitter, with
  corruption still present. All 44 tests pass. Fifteen request observations
  fall in resumed-P windows; each has another IDR within 40–90 ms, though
  other requests may also have triggered those IDRs. Retained for further
  comparison, not established as a root-cause fix.

- [x] **Scheduled format timestamp phase with gradual TSF correction — no visual improvement; reverted.**
  Timestamp each format from the intended slot cadence, gradually correcting
  phase toward sampled TSF by at most 4 us per format. Shared format/video
  timestamps remain intact; packet scheduling, PCM and recovery are unchanged.
  Real-reference adjacent format timestamp steps differ from a frame-period
  multiple by at most 7.33 us. The last Barista run has 73 steps off by at least
  1 ms, maximum 2,552.33 us. These are sender observations, not firmware phase
  errors. Tests cover jitter, drift, missing slots and 32-bit wrap. The separate
  runner fix lets analysis finish when Cemu closes after recording.
  Capture: `20261004-170921-_fvtpoux`, 87.58 seconds active video. Maximum
  timestamp step error is now 4.67 us; none reach 1 ms. All 44 tests pass and
  the host stream decodes without warnings. There are 918 recovery requests,
  versus 448 before, and 174 large frames versus 34; content differs.
  User reports roughly the same video. The phase clock is removed and the
  previous engine restored. Keep the separate runner analysis fix. This
  rules out retaining this phase-smoothing trial as a demonstrated fix.

- [x] **Skip discarded CAVLC residual serialization — retained provisionally.**
  CABAC frames also ran the CAVLC coefficient writer even though Barista
  discarded that output. Preserve nonzero counts for neighbor/deblocking
  state while skipping that writer only with an active CABAC context.
  Hypothesis: reduce transition encoding stalls without changing preset 5,
  motion search, quality, reconstruction or the transmitted bitstream.
  Compare the old/new CABAC bytes and reference pixels before live testing.
  The optimization is explicitly selected by NativeEncoder; diagnostic
  callers retain full CAVLC output. Old/new bytes and every reference pixel
  match on 48 colored transition frames. The corrected full suite passes
  all 44 tests. Benchmark mean improves about 4.2%, with both runs still over
  budget on its textured workload. Capture: `20261004-171821-38ft2_xv`,
  89.43 seconds active video. User reports slightly less corruption. Keep for
  further comparison; scene differences prevent calling it a proven fix.

- [x] **Release build of the retained engine — retained for framerate.**
  Previous installed experiments came from `build/`, configured as Debug.
  Build the same source in `build-gamepad-release/` with Release optimization;
  keep codec settings, protocol, pacing and recovery intact. Compare old/new
  CABAC bytes and reference pixels and run the full suite in the Release
  directory before installation. This is an encoding-headroom hypothesis;
  the textured Debug benchmark remained about 30 ms/frame after the last
  optimization. Preserve the Debug directory and a rollback engine.
  All 44 Release tests pass; CABAC bytes and reference pixels match Debug.
  Release benchmark mean is 4.28 ms, maximum 8.78 ms, with zero frames over
  budget. Capture: `20261004-172550-rqa5b99w`, 88.57 seconds active video;
  observed host cadence is 59.86 fps and logged encoding has zero over-budget
  frames. User reports much better framerate but lingering corruption/smearing.
  Use Release for further hardware trials; do not return to Debug by accident.
  Recovery requests drop from 278 to 14, with different scene content; this
  is not proof that the remaining visual artifacts are packet loss or fixed.

- [x] **Proactive IDR after 60 frames — retained for clearing smear.**
  Release improves framerate but user reports lingering smear. Force a clean
  reference after 60 encoded frames without an IDR; requested recovery stays
  immediate. Preserve the format-only slot, timing and codec settings.
  Hypothesis: bound propagation of corruption that does not trigger a pad
  recovery request. Expect extra IDR bandwidth and roughly one fewer video
  frame per second. Verify unsolicited IDRs and recovery slot ordering in
  the mock-radio integration test before the physical trial.
  All 45 Release tests pass. Capture: `20261004-173358-j7vgjwzm`, 85.56
  seconds active video, 4,969 complete host frames, 160 IDRs, 58.07 fps.
  77 IDR intervals are exactly 60 frames; none exceed 60. Logged encoding
  has no overruns; host reconstruction decodes without warnings. User:
  “Corruption still occers but does not linger and gets cleared.” Retain
  as a recovery mitigation, not a fix for the initiating corruption.
  Requests rise to 242 versus 14 in the previous run, with different content;
  many IDRs follow at three-frame distances. Extra resets have a cost.

- [x] **Disable fractional-pixel motion refinement — retained provisionally.**
  Keep preset 5, QP32, periodic IDRs and pacing. NativeEncoder explicitly
  disables only the fractional search step; ordinary MiniH264 diagnostics
  retain it. Hypothesis: motion prediction during transitions exposes a
  physical decoder compatibility difference that clean IDRs temporarily clear.
  This is unproven; integer luma motion still permits fractional chroma motion
  with 4:2:0 sampling. Expect changed residuals and potentially larger packets
  or reduced source quality. Require exact software decoder/reference equality
  and the full Release suite before a physical trial.
  Corrected full suite passes 45/45. Capture: `20261004-174301-d0oitm4b`,
  88.60 seconds active video, 5,145 complete host frames (166 IDR), 58.07
  fps, zero FFmpeg warnings and zero logged encoding overruns. Host first
  lateness p99 144 us, max 799 us; 249 radio recovery requests. Installed
  provisionally after the user reports “Hard to tell, seemed like an improvment
  tho, still corrupting.” This is tentative, not a demonstrated root-cause fix.
  Previous periodic-IDR Release engine is saved for rollback.

- [x] **Disable P-skip macroblocks — retained provisionally.**
  Keep integer luma motion, preset 5, fixed QP32, periodic IDRs and pacing.
  Disable early approximate skip and all-zero inter-to-skip conversion in
  NativeEncoder; ordinary zero-initialized diagnostics retain normal skip.
  Hypothesis: skipping small fade corrections or a physical skip-prediction
  difference starts artifacts that periodic IDRs clear. This is unproven.
  More residual analysis and explicit P coding may cost CPU and bandwidth.
  Require independent CAVLC/CABAC reconstruction equality and full Release
  tests before the physical comparison. Preserve the prior installed engine.
  All 45 Release tests pass. Capture: `20261004-175917-8xmqn001`, 87.60
  seconds active video, 5,088 complete host frames (163 IDR), 58.08 fps,
  zero decode warnings and logged encoding overruns. User: “Seems like an
  improvement but still corrupting in the same spots.” Retain provisionally;
  repeated artifact locations and 243 requests leave the cause unresolved.
  Synthetic fade error remains unchanged. Different content prevents treating
  lower large-frame count or tighter timing as proof of a skip-related fix.

- [x] **Zero-motion P prediction — retained; strong visual improvement.**
  Predict each P block from the same position in its reference; continue
  residual coding, intra selection, QP32, deblocking, periodic IDRs and pacing.
  Hypothesis: remaining motion/chroma interpolation differences initiate
  corruption; integer luma motion alone still permits fractional chroma.
  This is unproven and may increase residual size or reduce moving-scene
  quality. Select an explicit native-only create flag and align the diagnostic
  comparison. Require exact reference equality and all Release tests.
  All 45 tests pass. Capture: `20261004-180602-jo9dg6o_`, 88.72 seconds,
  5,148 complete host frames (170 IDR), 58.02 fps; no decode warnings or
  logged encoding overruns. User: “Omg, its pretty much all gone.” Keep as
  the comparison baseline. Requests remain 256 versus 243 previously; they
  do not track the reported artifacts. Motion vector coding, predictor state
  and interpolation remain possible causes; this trial does not isolate one.
  Follow-up: “I saw basically 0 corruption that time.” Record essentially
  zero observed corruption in this run, while leaving the root cause open.
  Saved engine: `/tmp/barista-engine-zero-motion-retained-20261004`.

Further candidates need a concrete hypothesis before becoming trials:

- Content-dependent transition load: colored fades and sliding graphics,
  encoded reference pixels, per-chunk sizes and delivery deadlines. User
  observations identify these transitions, but no timestamped visual markers
  yet correlate a specific transition with a specific captured frame.
- Timestamp cadence and phase across the IDR / format-only / P transition.
- Coalescing recovery requests around the first resumed P frame.
- Decoder consumption versus logical six-row chunk boundaries and timing.

For each completed trial, record the source change, engine hash, tests,
capture/runner paths, measured results, user's visual result and disposition.
Do not change several candidates together to chase a lower recovery counter.
