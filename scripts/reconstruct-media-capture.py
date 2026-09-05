#!/usr/bin/env python3
"""Reconstruct headerless DRH video as Annex B on stdout; diagnostics on stderr.

Uses the receiver headers in vanilla/lib/gamepad/video.c. This checks software
decodability, not physical GamePad compatibility. Incomplete frames invalidate
the reference chain until the next complete IDR. No credentials are accessed.
"""
import argparse
import collections
import importlib.util
from pathlib import Path
import struct
import sys

spec = importlib.util.spec_from_file_location('media', Path(__file__).with_name('analyze-media-capture.py'))
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


def escape(data):
    out = bytearray()
    zeros = 0
    for value in data:
        if zeros >= 2 and value <= 3:
            out.append(3)
            zeros = 0
        out.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return out


def reconstruct(path, output, idr_only=False):
    counts = collections.Counter()
    frame = None
    synchronized = False
    decode_number = 0
    previous = None
    start = bytes.fromhex('00000001')
    output.write(start + bytes.fromhex('67640020ac2b406c1ef368') +
                 start + bytes.fromhex('68ee060ce8'))
    for wall, ip in media.records(path):
        if len(ip) < 28 or ip[9] != 17:
            continue
        h = (ip[0] & 15) * 4
        if h < 20 or len(ip) < h + 8:
            continue
        _, port, length, _ = struct.unpack_from('!HHHH', ip, h)
        if port != 50120 or ip[6] & 0x3f or ip[7]:
            continue
        p = ip[h + 8:h + length]
        if len(p) < 16:
            continue
        identity = bytes(p)
        if identity == previous:
            counts['duplicate_packets'] += 1
            continue
        previous = identity
        seq = ((p[0] & 3) << 8) | p[1]
        timestamp = p[4:8]
        if p[2] & 0x40:
            if frame is not None:
                counts['incomplete_frames'] += 1
                synchronized = False
            frame = dict(data=bytearray(), stamp=timestamp, seq=seq,
                         chunks=0, valid=True, idr=0x80 in p[8:16])
        if frame is None:
            continue
        if seq != frame['seq'] or timestamp != frame['stamp'] or \
                ((p[2] & 7) << 8 | p[3]) != len(p) - 16:
            frame['valid'] = False
        frame['seq'] = (seq + 1) % 1024
        frame['data'].extend(p[16:])
        frame['chunks'] += bool(p[2] & 0x20)
        if not p[2] & 0x10:
            continue
        if not frame['valid'] or frame['chunks'] != 5:
            counts['incomplete_frames'] += 1
            synchronized = False
        else:
            idr = frame['idr']
            if idr:
                synchronized = True
                decode_number = 0
            if synchronized and (idr or not idr_only):
                header = 0x25b804ff if idr else 0x21e003ff | ((decode_number & 255) << 13)
                output.write(start + escape(header.to_bytes(4, 'big') + frame['data']))
                counts['idr' if idr else 'predicted'] += 1
            else:
                counts['skipped_frames'] += 1
            decode_number = (decode_number + 1) % 256
        frame = None
    if frame is not None:
        counts['incomplete_frames'] += 1
    return dict(counts)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    parser.add_argument('--idr-only', action='store_true')
    args = parser.parse_args()
    print(reconstruct(args.capture, sys.stdout.buffer, args.idr_only), file=sys.stderr)
