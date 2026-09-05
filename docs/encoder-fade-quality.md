# Encoder fade quality: measured default change

2026-09-05. No radio, TSF, PCM, format-slot, recovery, frame-type or packet-pacing
changes. QP remains 32. This improves encoded pixels; hardware acceptance is pending.

## Evidence and chosen settings

The Cemu color test produced visible white-fade artifacts without recovery
requests during either white cycle. PC decoding showed up to 13 luma levels of
spatial variation in frames whose source should be uniform. Settled white/black
frames were uniform. Valid H.264 syntax did not imply artifact-free pixels.

Default encoder changes:

- `analyse.b_fast_pskip = 0`: do not take the early P-skip shortcut; ordinary
  rate-distortion decisions can still choose skipped blocks.
- `analyse.b_psy = 0`: disable texture-oriented perceptual optimization, retaining
  the existing search preset and explicit effective chroma-QP offset of zero.

These are encoder decisions, not changes to the implicit receiver PPS or features
such as weighted prediction, B-frames, reference count, or transform size.
`DRCD_LEGACY_ENCODER_QUALITY=1` restores the previous two settings for direct
daemon/worker comparisons. The normal profile launcher pins the new default (`0`).

Tested alternatives at the same QP: psy alone, fast-P-skip alone, DCT decimation
off, trellis=2, zero quantizer deadzones, and combinations. Psy alone did not fix
the synthetic fade. P-skip off alone improved fade MSE by 38% with 3% more bytes,
but increased real-clip sizes by 5–7%. Disabling both psy and early P-skip retained
most of that fade gain while improving error AND size on all four real excerpts.
Disabling decimation as well was not selected: its extra quality came with a
larger byte increase (about 26% on the test2 5s excerpt).

## Measurements

Baseline means the previous psy/early-P-skip settings, not the newly updated
launcher's profile named `baseline`. The same decoded input frames and forced
initial-IDR/P chain were used for each comparison, with 59.94Hz source cadence.

| Source | Frames | Luma MSE, old → new | Encoded bytes, old → new |
| --- | ---: | ---: | ---: |
| Uniform white fade up/down | 240 | 5.971 → 3.771 | 163354 → 191932 |
| test2.mp4, starting 5s | 180 | 12.937 → 11.059 | 203160 → 187085 |
| test2.mp4, starting 18s | 180 | 7.671 → 5.967 | 178677 → 166951 |
| test1.mkv, starting 12s | 180 | 27.448 → 23.434 | 428963 → 395408 |
| test1.mkv, starting 45s | 180 | 16.658 → 14.198 | 270283 → 247683 |

The fade's mean macroblock-boundary error jump fell 0.172 → 0.131; its maximum
within-frame luma range only fell 12 → 11, and median range stayed 8. Therefore
this is a partial improvement, not elimination. Its maximum encoded frame stayed
3703 bytes. Real excerpts had lower maximum and p99 frame sizes with the new
settings. MSE is source-referenced squared error, not a perceptual quality score.
For real content, full-frame luma range reflects source detail and is NOT an
artifact metric. Boundary measurements subtract source pixels before comparing
adjacent blocks. Clip MSE includes the coded ten-column right-hand padding;
range/boundary metrics exclude it.

Measurements use the local patched encoder's reconstructed pixels. For the saved
240-frame fade and the 5s/12s excerpts, all production worker chunks matched the
probe bit-for-bit. The regression suite additionally compares reconstructed YUV
against FFmpeg decoding, in slow/fast and old/new modes. Saved MP4s contain the
measured stream without another lossy encode; all six strictly decode.

Review artifacts: `/tmp/drcd-fade-quality-ze8WNh/{fade,test1,test2}/` contains
`baseline.mp4`, `production.mp4`, their Annex-B H.264 and metric JSON files.
These are offline round trips, not new transmissions to the physical GamePad.

## Reproduce

Requires NumPy for the measurement tool; ordinary regression tests do not.
Use a new output directory (existing directories are rejected):

```sh
python scripts/measure-encoder-quality.py --profiles baseline production --output-dir /tmp/fade-review-new
python scripts/measure-encoder-quality.py --clip test2.mp4 --start 5 --frames 180 --profiles baseline production --output-dir /tmp/test2-review-new
```

Validation: build, all 18 CTest cases, 14 profile/rate-launcher tests, 300-frame
DRH stress decode, and strict decoding of the six saved review clips. The extended
reconstruction test requires at least 15% sampled fade-error improvement, limits
size growth to 35%, and verifies both effective settings and production bytes.
It does not replace subjective checking of the GamePad or a representative game.
