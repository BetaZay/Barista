# Native encoder validation

## Reconstruction

`drcd_minih264_cabac_reconstruction`, `drcd_minih264_planar_reconstruction`,
and `drcd_minih264_fast_reconstruction` decode 300 moving I420 pictures with
FFmpeg. They compare all coded 864x480 pixels with the encoder's internal
deblocked reference, check adapter output and chunk metadata, and cover a long
P-frame sequence, frame-number wrap, and consecutive forced IDRs. The coded
input is 864x480; the implicit GamePad SPS crops the visible width to 854.
Decoder cropping is disabled for the test so reference pixels in the right-hand
padding are verified too.

## Encoding performance

Reproduce an encoder-only benchmark (input generation and I/O are not timed):

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARISTA_BUILD_DESKTOP=OFF
cmake --build build-release --target drcd_native_encoder_benchmark --parallel
build-release/core/drcd_native_encoder_benchmark
build-release/core/drcd_native_encoder_benchmark fast
```

Measured on 2026-09-07, AMD Ryzen 5 7600, Linux x86_64, GCC 16.2.1 Release:

| Preset | Mean ms | P95 ms | P99 ms | Maximum ms | Frames over 16.6834 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Default | 4.513 | 4.661 | 5.014 | 7.026 | 0/600 |
| Fast | 4.037 | 4.188 | 4.383 | 7.259 | 0/600 |

The workload is a deterministic moving luma/chroma pattern with IDRs every 270
frames. These results establish headroom for this workload on this host, not
a worst-case guarantee or an end-to-end 59.94 FPS result. Timing includes input
copying, MiniH264 analysis, CABAC encoding, and owned chunk creation. It currently
also includes the diagnostic CAVLC path. Timing is reported rather than enforced
as a machine-dependent CTest assertion.

## Integration and verification scope

The production factory now selects the native adapter. The production replay
worker is checked byte-for-byte against a native reconstruction probe, then its
stream is independently decoded and compared with all reference pixels. Moving
chroma and luma fades exercise both default and fast search.

The 120-frame fade has approximately 19.91 sampled luma MSE at fixed QP32; the
regression ceiling is 25 (five luma levels RMS). This replaces the old comparison
between two x264-specific quality policies, not a claim of improved native
source fidelity. Decoder-to-reference equality remains exact.

The bundled legacy library and its obsolete reference probe have been removed.
The generated engine build has no `drc_x264`/`libx264` dependency, and the replay
worker contains the native encoder implementation. A fresh Ninja Debug build
with `BARISTA_BUILD_ENGINE=OFF` passes all six desktop-only tests (local socket
access is required for the single-instance test).

The environment-selectable native configuration exposes fast search and planar prediction.
`DRCD_VIDEO_QP`, `DRCD_QP28`, `DRCD_LEGACY_ENCODER_QUALITY`, and
`DRCD_INTRA_REFRESH` are retired encoder settings and have no effect. QP is fixed
at 32; recovery uses requested IDRs and a proactive reset after 60 frames,
not cyclic intra-refresh. The retained native adapter also selects explicit
zero-motion P coding; trial history and physical results are recorded below. Status messages
report those native behaviors rather than the former x264 policies.
No GamePad hardware playback or radio timing validation is claimed here.

The native adapter now explicitly skips unused CAVLC residual serialization,
while diagnostic callers retain complete CAVLC by default. An old/new native
binary comparison on 48 colored fade/slide frames, including consecutive
IDRs, preserves every CABAC byte and reference pixel for both presets. The
full reconstruction suite still verifies CAVLC diagnostics and native CABAC.
The 2026-10-04 hardware trial tentatively showed slightly less corruption;
see `docs/gamepad-recovery-experiment.md` for measured results and limits.

## Completion evidence

| Requirement | Evidence |
| --- | --- |
| Implicit SPS/PPS CABAC compatibility | FFmpeg decoding with the GamePad SPS/PPS and exact full-frame reference equality |
| 864x480 I420, IDR/P, QP32, configurable planar disabling | Native adapter parameters, DRH QP clamp, environment tests, and three reconstruction variants |
| Five six-row chunks from one continuous slice | `CabacSlice` records boundaries every 324 macroblocks using one writer/context set; it terminates only after 1,620 macroblocks |
| Motion, long reference chain, forced recovery | 300 moving frames, 269 consecutive P frames, frame-number wrap, and consecutive forced IDRs |
| Packet software compatibility | Media self-test checks format/VSTRM bytes and timestamps; packet scheduling and AppHook recovery tests exercise native output |
| 59.94 FPS encoding budget | Reproducible Release measurements above, scoped to this host and workload |
| Live backend replacement | `CreateEncoder` returns `NativeEncoder`; no tracked legacy source or generated x264 link dependency remains |
| Build/test gates | Final Debug build and all 33 CTests pass; fresh engine-disabled desktop build and all six tests pass |

These are Linux x86_64 software results. They do not certify other CPU paths,
GamePad firmware behavior, radio delivery, or end-to-end latency. Real-device
playback remains the next integration check, not a result of FFmpeg validation.


The 2026-10-04 integer-motion trial adds `disable_subpixel_motion_flag` to
MiniH264 create parameters, selecting it only in NativeEncoder. It bypasses
fractional-pixel refinement while retaining the preset's other search and
reconstruction behavior. Zero-initialized diagnostic callers retain normal
search. This tests an unproven physical decoder compatibility hypothesis;
see `docs/gamepad-recovery-experiment.md` for outcome and disposition.


The subsequent P-skip trial explicitly selects `disable_pskip_flag` in the
native adapter and its comparison probe. It disables early approximate skip
and zero-residual inter-to-skip conversion; ordinary callers default to zero.
It retains explicit inter prediction and normal quantization/reconstruction.
See the experiment report for physical results; this is a compatibility and
small-fade-correction hypothesis, not a known hardware defect.


The zero-motion trial selects `zero_motion_flag` only in NativeEncoder and
its comparison probe. It constructs an explicit same-position inter candidate
with an MVD cancelling its predictor, preserving residuals, intra selection
and reconstruction. Ordinary callers default to normal motion search.
This tests a remaining motion/chroma interpolation hypothesis; it may increase
moving-scene residuals and does not establish a hardware decoder defect.
