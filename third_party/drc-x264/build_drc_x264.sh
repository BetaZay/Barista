#!/usr/bin/env bash
set -euo pipefail

source_dir="$1"
build_dir="$2"
install_dir="$3"

mkdir -p "$build_dir" "$install_dir"

# This historical configure script only looks for the old executable name
# `yasm`. Modern NASM accepts the same x264 assembly sources and flags.
if ! command -v yasm >/dev/null 2>&1 && command -v nasm >/dev/null 2>&1; then
    ln -sf "$(dirname "$0")/yasm-wrapper.sh" "$build_dir/yasm"
    export PATH="$build_dir:$PATH"
fi

if [[ ! -f "$build_dir/config.mak" ]]; then
    cd "$build_dir"
    "$source_dir/configure" \
        --prefix="$install_dir" \
        --enable-static \
        --disable-cli \
        --disable-opencl \
        --disable-lavf \
        --disable-swscale \
        --disable-ffms \
        --disable-gpac \
        --disable-lsmash \
        --bit-depth=8 \
        --chroma-format=420
fi

make -C "$build_dir" -j"$(nproc)"
make -C "$build_dir" install
