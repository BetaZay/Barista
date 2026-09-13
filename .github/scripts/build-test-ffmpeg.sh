#!/usr/bin/env bash
# CI-only reference decoder. Never installed into Barista's RPM or runtime image.
set -euo pipefail
umask 022
prefix=${1:?Usage: build-test-ffmpeg.sh ABSOLUTE_INSTALL_PREFIX}
[[ "$prefix" == /* ]] || { echo "An absolute install prefix is required" >&2; exit 1; }
mkdir -p "$prefix/bin"
work=$(mktemp -d)

# Official FFmpeg n8.1.2, pinned to its peeled commit rather than a moving tag.
revision=38b88335f99e76ed89ff3c93f877fdefce736c13
git -C "$work" init -q
git -C "$work" fetch --depth 1 https://github.com/FFmpeg/FFmpeg.git "$revision"
git -C "$work" checkout --detach FETCH_HEAD
test "$(git -C "$work" rev-parse HEAD)" = "$revision"

cd "$work"
./configure --disable-everything --disable-autodetect --disable-doc --disable-debug \
    --disable-network --disable-ffplay --disable-ffprobe --disable-avdevice \
    --enable-ffmpeg --enable-decoder=h264 --enable-parser=h264 --enable-demuxer=h264 \
    --enable-encoder=rawvideo --enable-muxer=rawvideo --enable-protocol=file,pipe \
    --enable-filter=buffer,buffersink,null,format,scale
make -j4 ffmpeg
install -m755 ffmpeg "$prefix/bin/ffmpeg"
"$prefix/bin/ffmpeg" -hide_banner -h decoder=h264
