#!/usr/bin/env python3
"""Offline QP32 comparison against source pixels, using the DRH encoder probe.

Requires NumPy and FFmpeg. Does not start Wi-Fi or send to a physical GamePad.
Reports quality AND encoded size; reconstructed pixels are not hardware acceptance.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import time

import numpy as np

WIDTH, HEIGHT = 864, 480
RAW_SIZE = WIDTH * HEIGHT * 3 // 2
PROFILES = {
    'production': [],
    'baseline': [],
    'no-psy': ['no-psy'],
    'no-decimate': ['no-decimate'],
    'no-pskip': ['no-pskip'],
    'trellis2': ['trellis2'],
    'no-psy-no-decimate': ['no-psy', 'no-decimate'],
    'no-psy-trellis2': ['no-psy', 'trellis2'],
    'no-psy-no-pskip': ['no-psy', 'no-pskip'],
    'no-pskip-no-decimate': ['no-pskip', 'no-decimate'],
    'zero-deadzone': ['zero-deadzone'],
    'no-pskip-zero-deadzone': ['no-pskip', 'zero-deadzone'],
}


def source_frames(args):
    if args.clip:
        result = subprocess.run([
            'ffmpeg', '-v', 'error', '-ss', str(args.start), '-i', str(args.clip),
            '-an', '-vf', 'scale=854:480:flags=bicubic,pad=864:480:0:0,fps=60000/1001',
            '-frames:v', str(args.frames), '-pix_fmt', 'yuv420p', '-f', 'rawvideo', '-'],
            capture_output=True, check=True, timeout=120)
        raw = result.stdout
        if len(raw) != args.frames * RAW_SIZE:
            raise ValueError('Clip shorter than requested test')
        return raw
    raw = bytearray()
    # Uniform neutral-chroma fade. Every source frame is spatially constant.
    for index in range(args.frames):
        position = index / max(1, args.frames - 1)
        y = round(16 + 219 * (1 - abs(2 * position - 1)))
        raw.extend(bytes([y]) * (WIDTH * HEIGHT) + bytes([128]) * (RAW_SIZE // 3))
    return bytes(raw)


def measure(probe, profile, raw, frames, encoder=None, output_dir=None):
    wire = b''.join(bytes([index == 0]) + raw[index * RAW_SIZE:(index + 1) * RAW_SIZE]
                    for index in range(frames))
    started = time.monotonic()
    env = {**os.environ, 'DRCD_LEGACY_ENCODER_QUALITY': '0' if profile == 'production' else '1',
           'DRCD_VIDEO_QP': '32', 'DRCD_INTRA_REFRESH': '1', 'DRCD_FAST_ENCODE': '0'}
    result = subprocess.run([str(probe), 'stream', *PROFILES[profile]], input=wire,
                            capture_output=True, check=True, timeout=120,
                            env=env)
    elapsed = time.monotonic() - started
    settings = json.loads(result.stderr)
    if settings['effective_chroma_offset'] != 0:
        raise ValueError('Encoder violated implicit chroma PPS contract')
    worker = None
    if encoder and profile in ('baseline', 'production'):
        worker = subprocess.run([str(encoder)], input=wire, capture_output=True,
                                check=True, env=env, timeout=120).stdout
    offset, worker_offset, sizes, ranges, errors, boundaries = 0, 0, [], [], [], []
    encoded_frames = []
    chroma_errors = []
    for index in range(frames):
        size, = struct.unpack_from('<I', result.stdout, offset)
        offset += 4
        if not 0 < size <= 4 * 1024 * 1024:
            raise ValueError('Invalid encoded size')
        sizes.append(size)
        encoded = result.stdout[offset:offset + size]
        encoded_frames.append(encoded)
        if worker is not None:
            chunks = []
            for _ in range(5):
                chunk_size, = struct.unpack_from('<I', worker, worker_offset)
                worker_offset += 4
                if not 0 < chunk_size <= 4 * 1024 * 1024:
                    raise ValueError('Invalid production chunk size')
                chunks.append(worker[worker_offset:worker_offset + chunk_size])
                worker_offset += chunk_size
            if b''.join(chunks) != encoded:
                raise ValueError('Production worker differs from measured encoder')
        offset += size
        decoded = np.frombuffer(result.stdout, np.uint8, RAW_SIZE, offset)
        offset += RAW_SIZE
        source = np.frombuffer(raw, np.uint8, RAW_SIZE, index * RAW_SIZE)
        delta = decoded.astype(np.int16) - source.astype(np.int16)
        errors.append(float(np.mean(delta[:WIDTH * HEIGHT].astype(np.float64) ** 2)))
        chroma_errors.append(float(np.mean(delta[WIDTH * HEIGHT:].astype(np.float64) ** 2)))
        # Exclude the hidden right-hand padding, and measure error discontinuities
        # rather than real source edges at macroblock boundaries.
        y = decoded[:WIDTH * HEIGHT].reshape(HEIGHT, WIDTH)[:, :854]
        ranges.append(int(y.max()) - int(y.min()))
        e = delta[:WIDTH * HEIGHT].reshape(HEIGHT, WIDTH)[:, :854]
        jumps = np.concatenate((np.abs(e[:, 16::16] - e[:, 15:-1:16]).ravel(),
                                np.abs(e[16::16, :] - e[15:-1:16, :]).ravel()))
        boundaries.append(float(jumps.mean()))
    if offset != len(result.stdout):
        raise ValueError('Unexpected trailing probe output')
    if worker is not None and worker_offset != len(worker):
        raise ValueError('Unexpected trailing production output')
    if output_dir:
        spec = importlib.util.spec_from_file_location('reconstruct',
                Path(__file__).with_name('reconstruct-media-capture.py'))
        reconstruct = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reconstruct)
        annex = bytearray(b'\0\0\0\1' + bytes.fromhex('67640020ac2b406c1ef368') +
                          b'\0\0\0\1' + bytes.fromhex('68ee060ce8'))
        for index, encoded in enumerate(encoded_frames):
            header = 0x25b804ff if index == 0 else 0x21e003ff | ((index & 255) << 13)
            annex.extend(b'\0\0\0\1' + reconstruct.escape(header.to_bytes(4, 'big') + encoded))
        h264_path = output_dir / (profile + '.h264')
        with h264_path.open('xb') as stream:
            stream.write(annex)
        # Copy the measured bitstream: no second lossy encode in review videos.
        subprocess.run(['ffmpeg', '-v', 'error', '-n', '-r', '60000/1001', '-f', 'h264',
                        '-i', str(h264_path), '-c:v', 'copy', str(output_dir / (profile + '.mp4'))],
                       check=True, timeout=30)
    mse = float(np.mean(errors))
    return dict(profile=profile, frames=frames, wall_seconds=round(elapsed, 3),
                production_bytes_verified=worker is not None,
                y_mse=mse, y_psnr_db=None if mse == 0 else float(10 * np.log10(255 ** 2 / mse)),
                chroma_mse=float(np.mean(chroma_errors)),
                frame_luma_range_max=max(ranges), frame_luma_range_median=float(np.median(ranges)),
                macroblock_error_jump_mean=float(np.mean(boundaries)),
                bytes_total=sum(sizes), frame_bytes_max=max(sizes),
                frame_bytes_p99=float(np.percentile(sizes, 99)),
                settings=settings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, default=Path(__file__).resolve().parents[1] /
                        'build/drcd/drcd_encoder_recon_probe')
    parser.add_argument('--clip', type=Path)
    parser.add_argument('--encoder', type=Path, default=Path(__file__).resolve().parents[1] /
                        'build/drcd/drcd_reencode_replay')
    parser.add_argument('--output-dir', type=Path, help='new directory for review videos and metrics')
    parser.add_argument('--start', type=float, default=0)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--profiles', nargs='+', choices=PROFILES, default=list(PROFILES))
    args = parser.parse_args()
    if not 2 <= args.frames <= 600 or args.start < 0:
        parser.error('Use 2..600 frames and nonnegative start')
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=False)
    raw = source_frames(args)
    for profile in args.profiles:
        report = dict(source=str(args.clip) if args.clip else 'uniform-white-fade', start=args.start,
                      **measure(args.probe, profile, raw, args.frames, args.encoder, args.output_dir))
        line = json.dumps(report)
        print(line, flush=True)
        if args.output_dir:
            with (args.output_dir / (profile + '.json')).open('x') as stream:
                stream.write(line + '\n')


if __name__ == '__main__':
    main()
