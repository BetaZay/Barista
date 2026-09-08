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
| Default | 4.607 | 5.443 | 5.521 | 8.193 | 0/600 |
| Fast | 3.965 | 4.062 | 4.161 | 6.938 | 0/600 |

The workload is a deterministic moving luma/chroma pattern with IDRs every 270
frames. These results establish headroom for this workload on this host, not
a worst-case guarantee or an end-to-end 59.94 FPS result. Timing includes input
copying, MiniH264 analysis, CABAC encoding, and owned chunk creation. It currently
also includes the diagnostic CAVLC path. Timing is reported rather than enforced
as a machine-dependent CTest assertion.

## Remaining gates

The production factory now selects the native adapter. The production replay
worker is checked byte-for-byte against a native reconstruction probe, then its
stream is independently decoded and compared with all reference pixels. Moving
chroma and luma fades exercise both default and fast search.

The 120-frame fade has approximately 19.91 sampled luma MSE at fixed QP32; the
regression ceiling is 25 (five luma levels RMS). This replaces the old comparison
between two x264-specific quality policies, not a claim of improved native
source fidelity. Decoder-to-reference equality remains exact.

Legacy dependency removal and desktop-only checks remain necessary.
No GamePad hardware playback or radio timing validation is claimed here.
