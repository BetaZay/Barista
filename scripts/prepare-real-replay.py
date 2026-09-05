#!/usr/bin/env python3
"""Extract the longest intact IDR-led video chain plus format/PCM; no keys needed."""
import argparse
import importlib.util
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location("media", Path(__file__).with_name("analyze-media-capture.py"))
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


def prepare(path):
    packets, frames = [], []
    previous = {}
    frame = None
    chain = []
    chains = []
    last_seq = None
    for wall, ip in media.records(path):
        if len(ip) < 28 or ip[9] != 17 or ip[12:20] != bytes([192,168,1,10,192,168,1,11]):
            continue
        h = (ip[0] & 15) * 4
        if h < 20 or len(ip) < h + 8 or int.from_bytes(ip[6:8], 'big') & 0x3fff:
            continue
        _, port, length, _ = struct.unpack_from('!HHHH', ip, h)
        if port not in (50120, 50121) or length < 8 or h + length > len(ip):
            continue
        p = ip[h+8:h+length]
        if previous.get(port) == p:
            continue
        previous[port] = p
        if port == 50121:
            if len(p) == 32 and p[0] & 4:
                packets.append((wall, 1, p))
            elif len(p) == 1672 and not p[0] & 4:
                packets.append((wall, 2, p))
            continue
        if len(p) < 16:
            continue
        seq = ((p[0] & 3) << 8) | p[1]
        gap = last_seq is not None and seq != (last_seq + 1) % 1024
        last_seq = seq
        if gap or (p[2] & 64 and frame is not None):
            if chain: chains.append(chain)
            chain = []
            frame = None
        if p[2] & 64:
            frame = {'packets': [], 'stamp': p[4:8], 'idr': 128 in p[8:16], 'ends': 0, 'valid': True}
        if frame is None:
            continue
        frame['packets'].append((wall, 0, p))
        frame['ends'] += bool(p[2] & 32)
        frame['valid'] &= p[4:8] == frame['stamp'] and ((p[2] & 7) << 8 | p[3]) == len(p)-16
        if p[2] & 16:
            if frame['valid'] and frame['ends'] == 5:
                if frame['idr'] and not chain:
                    chain = [frame]
                elif chain:
                    chain.append(frame)
            else:
                if chain: chains.append(chain)
                chain = []
            frame = None
    if chain: chains.append(chain)
    if not chains:
        raise ValueError('No complete IDR-led reference chain; the Vanilla steady-state capture alone cannot bootstrap replay')
    chain = max(chains, key=lambda c: c[-1]['packets'][-1][0] - c[0]['packets'][0][0])
    first = chain[0]['packets'][0][0]
    # Limit the artifact to 30 seconds, ending at a complete frame.
    chain = [f for f in chain if f['packets'][-1][0] - first < 30_000_000]
    last = chain[-1]['packets'][-1][0]
    stamps = {f['stamp'] for f in chain}
    selected = [p for f in chain for p in f['packets']]
    formats = [p for p in packets if p[1] == 1 and p[2][8:12][::-1] in stamps and first-100000 <= p[0] <= last]
    if {p[2][8:12][::-1] for p in formats} != stamps:
        raise ValueError('Selected video chain lacks matching format packets')
    selected += formats
    selected += [p for p in packets if p[1] == 2 and first <= p[0] <= last]
    selected.sort(key=lambda p: p[0])
    origin = selected[0][0]
    result = bytearray(b'DRCREP01' + struct.pack('<III', len(selected), first-origin, int.from_bytes(chain[0]['stamp'], 'big')))
    for wall, kind, p in selected:
        result.extend(struct.pack('<IBH', wall-origin, kind, len(p)))
        result.extend(p)
    return result, {'frames': len(chain), 'idr_frames': sum(f['idr'] for f in chain),
                    'packets': len(selected), 'duration_s': (last-origin)/1e6,
                    'pcm_packets': sum(p[1] == 2 for p in selected)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    parser.add_argument('output')
    args = parser.parse_args()
    artifact, summary = prepare(args.capture)
    # Generated binary artifact, not a source edit. Never overwrite a previous run.
    with open(args.output, 'xb') as stream:
        stream.write(artifact)
    print(summary)
