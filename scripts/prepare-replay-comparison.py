#!/usr/bin/env python3
"""Prepare real-packet, re-encoded-packet and live-engine inputs from one capture."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


prepare = module('prepare', 'prepare-real-replay.py')
reencode = module('reencode', 'reencode-real-replay.py')


def verify_frames(path, expected, log):
    result = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror',
                             '-err_detect', 'explode', '-apply_cropping', '0',
                             '-i', str(path), '-map', '0:v:0', '-f', 'framemd5', '-'],
                            capture_output=True, text=True, timeout=120)
    log.write_text(result.stderr)
    if result.returncode or result.stderr:
        raise ValueError(f'Strict decoding failed: see {log}')
    frames = sum(bool(line.strip()) and not line.startswith('#')
                 for line in result.stdout.splitlines())
    if frames != expected:
        raise ValueError(f'Decoded {frames} frames from {path}; expected {expected}')
    path.with_suffix(path.suffix + '.framemd5').write_text(result.stdout)
    return frames


def build(capture, folder, encoder, seconds):
    # Exclusive directory: partial failures remain reviewable, prior trials intact.
    folder.mkdir(mode=0o700)
    original = folder / 'original.drcrep'
    encoded = folder / 'reencoded.drcrep'
    artifact, extracted = prepare.prepare(capture, seconds)
    original.write_bytes(artifact)
    changed = reencode.convert(original, encoded, encoder)
    _, original_records = reencode.read_replay(original)
    _, encoded_records = reencode.read_replay(encoded)
    if [r for r in original_records if r[1]] != [r for r in encoded_records if r[1]]:
        raise ValueError('Re-encoding changed audio or format records')
    for name, records in [('original', original_records), ('reencoded', encoded_records)]:
        path = folder / f'{name}.h264'
        with path.open('xb') as stream:
            count = reencode.write_annex_b(records, stream)
        verify_frames(path, count, folder / f'{name}-decode.log')
    # FFV1 retains all uncropped decoded columns. The live --play pipeline can
    # consume this without accidentally stretching the visible 854 columns.
    media = folder / 'source.mkv'
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-n',
                    '-apply_cropping', '0', '-i', str(folder / 'original.h264'),
                    '-an', '-c:v', 'ffv1', '-pix_fmt', 'yuv420p', str(media)],
                   check=True, timeout=120)
    verify_frames(media, extracted['frames'], folder / 'source-decode.log')
    # Require exact pixel agreement, not just a successful FFV1 decode.
    def pixels(path):
        lines = path.read_text().splitlines()
        return [line.rsplit(',', 1)[1].strip() for line in lines if line and not line.startswith('#')]
    if pixels(folder / 'original.h264.framemd5') != pixels(folder / 'source.mkv.framemd5'):
        raise ValueError('Live-engine source pixels differ from decoded real video')
    files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
             for p in (original, encoded, folder / 'original.h264', folder / 'reencoded.h264', media)}
    report = {'capture': str(capture.resolve()),
              'capture_sha256': hashlib.sha256(capture.read_bytes()).hexdigest(),
              'encoder': str(encoder.resolve()),
              'encoder_sha256': hashlib.sha256(encoder.read_bytes()).hexdigest(),
              'extracted': extracted, 'reencoded': changed, 'sha256': files,
              'validation': 'Both H.264 streams strictly decode; FFV1 pixels match original exactly',
              'limits': ['Replay translates timestamps/sequences; it does not replay encrypted Wi-Fi frames',
                         'Re-encoded packet sizes/counts may differ; audio/format and chunk time windows retained',
                         'Loop seams restart with IDR; analyze seams separately',
                         'Live media adds normal scheduling/recovery and regenerates audio/format packets']}
    (folder / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='Authenticated decrypted IP pcap')
    parser.add_argument('output', type=Path, help='New output directory')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--encoder', type=Path,
                        default=Path(__file__).resolve().parent.parent / 'build/app/drcd_reencode_replay')
    args = parser.parse_args()
    print(json.dumps(build(args.capture, args.output, args.encoder, args.seconds), indent=2))


if __name__ == '__main__':
    main()
