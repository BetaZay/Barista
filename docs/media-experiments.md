# Checkpointed media experiments

Baseline commit: `34b4866`, tag `baseline-before-media-experiments`.
The launcher name `baseline` means current defaults, now the finalized-slice
DRH encoder v2, not a checkout of this historical commit. See CURRENT_STATUS.md.
This preserves the pipelined sender, QP32 and AP TSF implementation, **not an
artifact-free hardware configuration**. The user reported worse blocking with
intra-refresh disabled; the baseline and launcher keep it enabled.

Patched x264 sources are now tracked directly in this repository. Its original
nested Git metadata is preserved locally at `checkpoints/x264-original.git`.
Credentials, captures, media, build products and checkpoints are ignored.
Cemu and the separate EEPROM/analysis project are outside this checkpoint.

## Run one observed test at a time

From drc-project, keep the same Cemu scene and run each for 45–60 seconds,
then Ctrl-C and power-cycle the GamePad between tests. Do not run simultaneous
daemons or clock probes. Cemu still needs its usual CEMU_DRCD_SOCKET setting.

```sh
sudo bash scripts/test-media-profile.sh baseline cemu
sudo bash scripts/test-media-profile.sh all-idr cemu
sudo bash scripts/test-media-profile.sh audio-time cemu
```

Each invocation prints its exact settings and creates timestamped `/tmp`
radio and companion IP captures, a separate `.log`, and first-IDR artifacts.
An optional third argument sets a specific pcap filename; existing captures,
logs and artifact directories are not reused. Unlike old runs, the daemon log
is alongside this capture, not `/tmp/drcd.log`.

| Profile | Difference from baseline | What it tests |
| --- | --- | --- |
| baseline | None | Reproducibility/control |
| all-idr | Encode every frame as a genuine IDR | Remove inter-picture reference dependencies |
| audio-time | PCM timestamp = AP TSF minus 10,000 us | Reference-console audio timing; size and cadence unchanged |
| combined | Both above | Interaction; run after isolated tests |
| burst | Existing chunk pacing disabled | Packet scheduling comparison |

Replace `cemu` with `generated` to use the existing moving grayscale pattern
and tone without Cemu/FFmpeg. Compare baseline versus all-idr with the same
source; changing both at once confounds the comparison.

AP TSF is forced on for this verified adapter: startup must fail if unavailable.
QP32, five-chunk DRH encoding, IDR-init flags, video send-time stamping,
416-frame PCM packets, normal keepalive and the bounded serial sender remain
unchanged except where explicitly listed. Old media experiment environment
overrides are overwritten. Ordinary drcd defaults remain unchanged.

All-IDR increases encode cost and radio traffic, potentially reducing cadence;
a worse result would not rule out reference-chain problems. Cleaner all-IDR
video would suggest dependencies/recovery matter, but would not prove the
encoder alone is responsible. MAC ACKs and valid FFmpeg decoding cannot establish
successful presentation by the physical GamePad.

## Local checks and recovery

```sh
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python scripts/test-media-profile-tests.py
```

Loopback tests cover normal, all-IDR, audio-time and combined packet streams,
including PCM timestamp age/size, video flags, sequence continuity, chunk timing,
audio and input. These checks do not replace physical acceptance testing.

To build the checkpoint without discarding current experiments, create a
separate worktree (choose an unused destination) and follow its build README:

```sh
git worktree add ../drc-project-baseline baseline-before-media-experiments
```

Do not reset the current worktree to switch a hardware test: the launcher’s
`baseline` profile already disables the new experiments.
