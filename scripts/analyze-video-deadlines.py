#!/usr/bin/env python3
"""Measure complete host video frames against their captured preceding format.

Capture times are observations, not exact scheduler execution or pad receipt.
The inferred video deadline is first format observation plus the configured
lead. Reports describe size and lateness; they do not identify visual scenes.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct


spec = importlib.util.spec_from_file_location('media', Path(__file__).with_name('analyze-media-capture.py'))
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


def distribution(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return {'count': len(values), 'min': values[0], 'median': values[len(values) // 2],
            'p99': values[int((len(values) - 1) * .99)], 'max': values[-1]}


def summarize(rows):
    return {'frames': len(rows),
            'first_packet_lateness_us': distribution([r['late'] for r in rows]),
            'second_chunk_end_us': distribution([r['second'] for r in rows]),
            'last_chunk_end_us': distribution([r['last'] for r in rows])}


def analyze(path, lead_us=5000):
    formats, rows = {}, []
    previous = frame = None
    incomplete = 0
    for wall, ip in media.records(path):
        if len(ip) < 28 or ip[9] != 17 or ip[12:20] != bytes([192, 168, 1, 10, 192, 168, 1, 11]):
            continue
        h = (ip[0] & 15) * 4
        if h < 20 or len(ip) < h + 8 or int.from_bytes(ip[6:8], 'big') & 0x3fff:
            continue
        _, port, length, _ = struct.unpack_from('!HHHH', ip, h)
        if length < 8 or h + length > len(ip):
            continue
        packet = ip[h + 8:h + length]
        if port == 50121 and len(packet) == 32 and packet[0] & 4:
            formats.setdefault(packet[8:12][::-1], wall)
        if port != 50120 or len(packet) < 16 or packet == previous:
            continue
        previous = packet
        sequence = ((packet[0] & 3) << 8) | packet[1]
        if packet[2] & 64:
            incomplete += frame is not None
            frame = {'first': wall, 'stamp': packet[4:8], 'next': sequence,
                     'ends': [], 'bytes': 0, 'valid': True}
        if frame is None:
            continue
        frame['valid'] &= packet[4:8] == frame['stamp'] and sequence == frame['next'] and \
                          ((packet[2] & 7) << 8 | packet[3]) == len(packet) - 16
        frame['next'] = (sequence + 1) & 1023
        frame['bytes'] += len(packet) - 16
        if packet[2] & 32:
            frame['ends'].append(wall)
        if packet[2] & 16:
            if frame['valid'] and len(frame['ends']) == 5 and frame['stamp'] in formats:
                deadline = formats[frame['stamp']] + lead_us
                rows.append({'bytes': frame['bytes'], 'late': frame['first'] - deadline,
                             'second': frame['ends'][1] - deadline,
                             'last': frame['ends'][-1] - deadline})
            else:
                incomplete += 1
            frame = None
    incomplete += frame is not None
    return {'all': summarize(rows),
            'large_frames_at_least_15000_bytes': summarize([r for r in rows if r['bytes'] >= 15000]),
            'starts_at_least_1000us_late': summarize([r for r in rows if r['late'] >= 1000]),
            'second_chunk_end_after_9750us': sum(r['second'] > 9750 for r in rows),
            'incomplete_or_unmatched_frames': incomplete,
            'limitations': 'Host observations only. Inferred deadline uses first matching format plus lead; '
                           'not scheduler execution or GamePad delivery. Packet capture must be shorter '
                           'than timestamp wrap. Large compressed frames are not identified transitions.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--lead-us', type=int, default=5000)
    args = parser.parse_args()
    print(json.dumps(analyze(args.capture, args.lead_us), indent=2))
