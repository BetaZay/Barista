#!/usr/bin/env bash
# Each invocation is one manually observed hardware test, never a channel scan.
set -euo pipefail
usage() {
    echo "Usage: sudo bash scripts/test-media-profile.sh PROFILE [apphook|generated] [output.pcap]"
    echo "Profiles: baseline, all-idr, audio-time, combined, burst"
    echo "Optional DRCD_TEST_DRY_RUN=1 prints the command without starting hardware."
}
if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
profile=${1:-baseline}
source_mode=${2:-apphook}
output=${3:-/tmp/drcd-${profile}-${source_mode}-$(date +%Y%m%d-%H%M%S).pcap}
run_log=${output%.pcap}.log
run_dump=${output%.pcap}-artifacts
if (($# > 3)); then usage >&2; exit 2; fi
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Pin the verified baseline, even if the calling shell has old experiments exported.
# Hardware clock failure must stop startup, never silently use RX/free-run time.
settings=(DRCD_AP_TSF_CLOCK=1 DRCD_VIDEO_QP=32 DRCD_QP28=0
    DRCD_SEND_TIME_VIDEO=1 DRCD_IDR_INIT=1 DRCD_CHUNK_PACING=1
    DRCD_INTRA_REFRESH=1 DRCD_FAST_ENCODE=0 DRCD_REFERENCE_VIDEO_OPTIONS=0
    DRCD_LEGACY_ENCODER_QUALITY=0
    DRCD_ALL_IDR=0 DRCD_REFERENCE_AUDIO_TIME=0 DRCD_GENERATED_AV=0
    "DRCD_LOG_FILE=$run_log" "DRCD_MEDIA_DUMP_DIR=$run_dump")
case "$profile" in
    baseline) ;;
    all-idr) settings+=(DRCD_ALL_IDR=1) ;;
    audio-time) settings+=(DRCD_REFERENCE_AUDIO_TIME=1) ;;
    combined) settings+=(DRCD_ALL_IDR=1 DRCD_REFERENCE_AUDIO_TIME=1) ;;
    burst) settings+=(DRCD_CHUNK_PACING=0) ;;
    *) usage >&2; exit 2 ;;
esac
arguments=(--np)
case "$source_mode" in
    apphook) settings+=(BARISTA_MUG_SOCKET=/tmp/drcd-media.sock) ;;
    generated) settings+=(DRCD_GENERATED_AV=1); arguments+=(--black) ;;
    *) usage >&2; exit 2 ;;
esac
command=(env -u BARISTA_MUG_SOCKET -u DRCD_REAL_REPLAY "${settings[@]}" bash "$project_dir/scripts/capture-drcd-session.sh" "$output" "${arguments[@]}")
printf 'Profile=%s source=%s; AP TSF required; Ctrl-C after 45-60 seconds of the same scene.\n' "$profile" "$source_mode"
printf '%q ' "${command[@]}"; printf '\n'
if [[ ${DRCD_TEST_DRY_RUN:-0} == 1 ]]; then exit 0; fi
if ((EUID != 0)); then echo "Run with sudo; no hardware was changed." >&2; exit 1; fi
if [[ -e $run_log || -e $run_dump ]]; then
    echo "Refusing to reuse log/artifact paths; choose a new output name." >&2; exit 1
fi
exec "${command[@]}"
