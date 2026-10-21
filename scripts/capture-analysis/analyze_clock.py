#!/usr/bin/env python3
"""Read-only timing comparison. Does not decode or display credentials.

Radiotap TSFT belongs to the capturing radio, not automatically to the AP.
Only received GamePad data frames anchor the clock; TX reports may be delayed.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('media_analyzer',
    Path(__file__).resolve().parent / 'analyze-media-capture.py')
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


def summary(values):
    if not values:
        return {'count': 0}
    values = sorted(values)
    return {'count': len(values), 'min': values[0], 'p10': values[len(values)//10],
            'median': values[len(values)//2], 'p90': values[len(values)*9//10],
            'max': values[-1]}


def radio_records(path):
    formats = {b'\xd4\xc3\xb2\xa1': ('<', 1000000),
               b'\xa1\xb2\xc3\xd4': ('>', 1000000),
               b'\x4d\x3c\xb2\xa1': ('<', 1000000000),
               b'\xa1\xb2\x3c\x4d': ('>', 1000000000)}
    with open(path, 'rb') as f:
        h = f.read(24)
        if len(h) != 24 or h[:4] not in formats:
            raise ValueError('Expected classic pcap')
        endian, scale = formats[h[:4]]
        if struct.unpack_from(endian + 'I', h, 20)[0] & 0xffff != 127:
            raise ValueError('Expected radiotap')
        while ph := f.read(16):
            if len(ph) != 16:
                raise ValueError('Truncated record header')
            sec, frac, n, _ = struct.unpack(endian + 'IIII', ph)
            if n > 16 * 1024 * 1024:
                raise ValueError('Oversized record')
            p = f.read(n)
            if len(p) != n:
                raise ValueError('Truncated record')
            if n < 8:
                continue
            rt = struct.unpack_from('<H', p, 2)[0]
            if rt < 8 or rt > n:
                continue
            present = word = struct.unpack_from('<I', p, 4)[0]
            offset = 8
            while word & 0x80000000:
                if offset + 4 > rt:
                    raise ValueError('Truncated radiotap bitmap')
                word = struct.unpack_from('<I', p, offset)[0]
                offset += 4
            tsf = None
            if present & 1:
                offset = (offset + 7) // 8 * 8
                if offset + 8 > rt:
                    raise ValueError('Truncated TSFT')
                tsf = struct.unpack_from('<Q', p, offset)[0]
                offset += 8
            flags = p[offset] if present & 2 and offset < rt else 0
            frame = p[rt:]
            if flags & 0x10:
                frame = frame[:-4]
            if flags & 0x40:
                continue
            yield sec * 1000000 + frac * 1000000 // scale, tsf, frame


def analyze(radio, ip_path, ap, pad):
    samples = []
    offsets = []
    tids = Counter()
    retries = Counter()
    for wall, tsf, frame in radio_records(radio):
        if len(frame) < 24:
            continue
        if frame[0] == 0x80 and frame[10:16] == ap and len(frame) >= 32 and tsf is not None:
            offsets.append(struct.unpack_from('<Q', frame, 24)[0] - tsf)
        if frame[0] & 0x0c != 8:
            continue
        if frame[10:16] == pad and frame[4:10] == ap and tsf:
            samples.append((wall, tsf))
        if frame[10:16] == ap and frame[4:10] == pad:
            retries['reported_ap_data'] += 1
            retries['retry_bit_set'] += bool(frame[1] & 8)
            if frame[0] & 0x80:
                q = 30 if frame[1] & 3 == 3 else 24
                if len(frame) >= q + 2:
                    tids[frame[q] & 15] += 1
    samples.sort()
    times = [v[0] for v in samples]
    residuals = [b[1] - a[1] - (b[0] - a[0]) for a, b in zip(samples, samples[1:])]
    ages = defaultdict(list)
    by_second = defaultdict(lambda: defaultdict(list))
    transitions = []
    current = None
    first = None
    stale = 0
    for wall, ip in media.records(ip_path):
        if len(ip) < 28 or ip[9] != 17:
            continue
        h = (ip[0] & 15) * 4
        if h < 20 or len(ip) < h + 8:
            continue
        _, port, size, _ = struct.unpack_from('!HHHH', ip, h)
        p = ip[h + 8:h + size]
        if first is None:
            first = wall
        if port == 50022 and len(p) == 128 and current != p[83] & 1:
            current = p[83] & 1
            transitions.append({'second': (wall-first)/1e6, 'waiting': current})
        if port == 50120 and len(p) >= 16 and p[2] & 64:
            kind, stamp = 'video', int.from_bytes(p[4:8], 'big')
        elif port == 50121 and len(p) > 32 and not p[0] & 4:
            kind, stamp = 'pcm', int.from_bytes(p[4:8], 'little')
        else:
            continue
        i = bisect.bisect_right(times, wall) - 1
        if i < 0 or wall-times[i] > 100000:
            stale += 1
            continue
        estimated = samples[i][1] + wall-times[i]
        age = ((estimated-stamp + 2**31) % 2**32) - 2**31
        ages[kind].append(age)
        by_second[int((wall-first)/1e6)][kind].append(age)
    correction = sorted(offsets)[len(offsets)//2] if offsets else None
    return {'radio': str(radio), 'rx_clock_anchors': len(samples),
            'unanchored_media_samples': stale, 'ap_minus_monitor_tsf_us': summary(offsets),
            'rx_tsf_vs_capture_interval_residual_us': summary(residuals),
            'media_age_in_monitor_clock_us': {k: summary(v) for k, v in ages.items()},
            'media_age_in_ap_clock_us': ({k: summary([x+correction for x in v])
                for k, v in ages.items()} if correction is not None else None),
            'ap_qos_tids': dict(tids), 'ap_retry_observations': dict(retries),
            'state_transitions': transitions,
            'per_second_age_us': {s: {k: summary(v) for k, v in fields.items()}
                                  for s, fields in by_second.items()},
            'limitations': 'RX clock extrapolation uses capture wall time, not app recv time. '
                'No AP-clock calibration without observed beacons. Same-adapter TX reports '
                'do not prove delivery, ACK rate, loss rate, or airtime. Retry observations '
                'are not deduplicated. Beacon correction assumes a stable single clock epoch.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('radio')
    parser.add_argument('ip_capture')
    parser.add_argument('--ap', default='40:d2:8a:bf:fc:a8')
    parser.add_argument('--pad', default='40:d2:8a:ab:90:00')
    args = parser.parse_args()
    print(json.dumps(analyze(args.radio, args.ip_capture,
                            bytes.fromhex(args.ap.replace(':', '')),
                            bytes.fromhex(args.pad.replace(':', ''))), indent=2))
