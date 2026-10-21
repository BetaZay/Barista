# minih264 upstream snapshot

- Repository: https://github.com/lieff/minih264
- Commit: `b0baea7a80ef9d12da97301dd1099b8791b5ba43`
- Commit date: 2020-12-10
- Source SHA-256: `661afe7802b4e1174e8389632f3ee437b903e036a9c91a7f035830947c6a7aae`
- License SHA-256: `6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01`

The initial snapshot was imported unchanged. The source hash above identifies that
original snapshot, not the current fork. Subsequent commits add C++20 integration,
DRH configuration, constrained intra prediction, and the continuous CABAC sink.

The native adapter's `EncoderOptions::disablePlanarPrediction` defaults to `true`
for GamePad compatibility. Setting it to `false` permits planar prediction;
software reconstruction tests exercise both settings. This does not establish
that a GamePad supports planar prediction. The production factory selects the
native adapter, using the compatibility default unless configured otherwise.
`DRCD_DISABLE_PLANAR_PREDICTION=0` permits planar prediction in that factory;
leave it unset for GamePad use.

When the native adapter opts into `skip_unused_cavlc_residual_flag` with a
DRH CABAC context, the residual path now updates nonzero
counts without serializing the unused CAVLC coefficient data. Motion search,
quantization, CABAC emission and reconstruction remain active. The separate
CAVLC output argument is diagnostic scaffolding in this mode and is not a
decodable stream. Diagnostic callers leave this flag zero to retain complete
CAVLC alongside CABAC. Ordinary encoding without a CABAC context retains CAVLC.


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
