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
that a GamePad supports planar prediction. The legacy live backend does not use
this new option yet.
