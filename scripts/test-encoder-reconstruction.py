#!/usr/bin/env python3
"""Check DRH decoded pixels against the native encoder's internal references.

The probe must also reproduce the production worker's CABAC bytes exactly.
No radio, credentials, NumPy, or firmware required. Tests slow and fast search.
"""
import argparse
import importlib.util
from pathlib import Path
import struct
import subprocess

spec = importlib.util.spec_from_file_location('reencode', Path(__file__).with_name('reencode-real-replay.py'))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
RAW_SIZE = 864 * 480 * 3 // 2


def take(data, offset, size):
    if size < 0 or offset + size > len(data):
        raise ValueError('Truncated encoder output')
    return data[offset:offset + size], offset + size


def sized(data, offset):
    header, offset = take(data, offset, 4)
    size, = struct.unpack('<I', header)
    if not 0 < size <= 4 * 1024 * 1024:
        raise ValueError('Invalid encoded chunk size')
    return take(data, offset, size)


def check(args, source, count, fast):
    env = {**r.encoder_settings(), 'DRCD_FAST_ENCODE': '1' if fast else '0'}
    probe = subprocess.run([str(args.probe), 'stream'] + (['fast'] if fast else []),
                           input=source, capture_output=True, check=True, env=env, timeout=30)
    worker = subprocess.run([str(args.encoder)], input=source, capture_output=True,
                            check=True, env=env, timeout=30)
    reference = bytearray()
    start = b'\0\0\0\1'
    annex = bytearray(start + bytes.fromhex('67640020ac2b406c1ef368') +
                      start + bytes.fromhex('68ee060ce8'))
    probe_pos = worker_pos = 0
    for index in range(count):
        encoded, probe_pos = sized(probe.stdout, probe_pos)
        pixels, probe_pos = take(probe.stdout, probe_pos, RAW_SIZE)
        reference.extend(pixels)
        chunks = []
        for _ in range(5):
            chunk, worker_pos = sized(worker.stdout, worker_pos)
            chunks.append(chunk)
        if encoded != b''.join(chunks):
            raise ValueError('Probe differs from production worker at frame ' + str(index))
        header = 0x25b804ff if index == 0 else 0x21e003ff | ((index & 255) << 13)
        annex.extend(start + r.reconstruct.escape(header.to_bytes(4, 'big') + encoded))
    if probe_pos != len(probe.stdout) or worker_pos != len(worker.stdout):
        raise ValueError('Unexpected trailing encoder output')
    decoded = subprocess.run([args.ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-err_detect',
        'explode', '-apply_cropping', '0', '-f', 'h264', '-i', 'pipe:0', '-pix_fmt', 'yuv420p',
        '-f', 'rawvideo', 'pipe:1'], input=annex, capture_output=True, check=True, timeout=30)
    if decoded.stdout != reference:
        raise ValueError('Decoded YUV pixels differ from internal reconstructed references')
    print(f'native {"fast" if fast else "default"}: '
          f'{count} frames, production bytes and all reconstructed YUV pixels match; chroma=0')
    return reference, len(worker.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--encoder', required=True, type=Path)
    parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    source = bytearray()
    for index in range(48):
        y = bytes(32 + ((x + index * 5) // 8) % 160 for x in range(864)) * 480
        u = bytes(72 + ((x // 8 + index) % 112) for x in range(432)) * 240
        v = bytes(184 - ((x // 16 + index * 3) % 112) for x in range(432)) * 240
        source.extend(bytes([index == 0]) + y + u + v)
    for fast in (False, True):
        check(args, source, 48, fast)

    # Reproduce the measured fade defect with source pixels, not just valid syntax.
    # Sample across the full luma plane; keep this regression free of NumPy.
    count = 120
    source = bytearray()
    levels = []
    for index in range(count):
        level = round(16 + 219 * (1 - abs(2 * index / (count - 1) - 1)))
        levels.append(level)
        source.extend(bytes([index == 0]) + bytes([level]) * (864 * 480) +
                      bytes([128]) * (864 * 480 // 2))
    for fast in (False, True):
        decoded, encoded_size = check(args, source, count, fast)
        error = sum((value - levels[index]) ** 2 for index in range(count)
                    for value in decoded[index * RAW_SIZE:index * RAW_SIZE + 864 * 480:16])
        mse = error / (count * 864 * 480 // 16)
        # An absolute source-fidelity bound replaces the comparison between two
        # obsolete x264 quality policies. Retain the actual fade workload.
        # Native QP32 currently measures 19.91 MSE on this workload. This
        # regression ceiling is five luma levels RMS, not lossless fidelity.
        if mse > 25:
            raise ValueError(f'Fade luma MSE exceeds 25: {mse}')
        print(f'Fade sampled MSE={mse:.4f}; encoded bytes={encoded_size}')


if __name__ == '__main__':
    main()
