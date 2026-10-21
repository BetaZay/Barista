#!/usr/bin/env python3
"""Compare format-message age with the firmware's 8000-tick freshness gate.

Read-only; no credentials or decrypted payloads enter the report. Use either an
existing decrypted companion of the SAME radio capture, or saved credentials
to authenticate that capture locally. A truncated final record is reported.
"""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import statistics
import struct
import sys

def signed_delta(a, b):
    return ((a - b + 2**31) % 2**32) - 2**31


def distribution(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return {'count': len(values), **{name: values[int((len(values) - 1) * q)]
            for name, q in [('min', 0), ('p10', .1), ('median', .5),
                            ('p90', .9), ('p99', .99), ('max', 1)]}}

import analyze_clock as clock
import decrypt_capture as decrypt
from analyze_delivery import udp

LLC_IP = bytes.fromhex('aaaa030000000800')


def fresh_at_counter(counter, timestamp):
    # Firmware 0x30ef0..0x30ef8 uses an UNSIGNED subtraction and comparison.
    return (counter - timestamp) % 2**32 < 8000


def checksum_valid(data):
    if len(data) % 2:
        data += b'\0'
    value = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
    while value >> 16:
        value = (value & 0xffff) + (value >> 16)
    return value == 0xffff


def checksum_status(ip):
    # Only complete, unfragmented UDP datagrams accepted by the shared parser.
    if udp(ip) is None:
        return None
    ihl = (ip[0] & 15) * 4
    length = int.from_bytes(ip[ihl + 4:ihl + 6], 'big')
    segment = ip[ihl:ihl + length]
    pseudo = ip[12:20] + bytes((0, 17)) + length.to_bytes(2, 'big')
    return ('valid' if checksum_valid(ip[:ihl]) else 'invalid',
            'omitted' if segment[6:8] == b'\0\0' else
            'valid' if checksum_valid(pseudo + segment) else 'invalid')


def prefix(iterator, counts, label):
    try:
        yield from iterator
    except ValueError as error:
        if str(error) not in ('Truncated pcap packet', 'Truncated pcap record',
                              'Truncated record', 'Truncated record header'):
            raise
        counts[label + '_truncated_tail'] += 1


def packet_key(wall, packet):
    # Companion decryption preserves addresses, sequence control and wall time.
    return wall, packet[4:24]


def analyze(args):
    counts = Counter()
    ap = bytes.fromhex(args.ap.replace(':', ''))
    pad = bytes.fromhex(args.pad.replace(':', ''))
    companions, keys = {}, {}
    if args.decrypted_radio:
        for sec, us, packet, flags in prefix(decrypt.records(args.decrypted_radio), counts, 'companion'):
            parsed = decrypt.data_frame(packet, flags)
            if parsed and packet[10:16] == ap and packet[4:10] == pad:
                key = packet_key(sec * 1000000 + us, packet)
                body = packet[parsed[1]:]
                if key in companions and companions[key] != body:
                    raise ValueError('Ambiguous decrypted companion record')
                companions[key] = body
    else:
        fields = dict(line.rstrip('\n').split('=', 1) for line in
                      args.credentials.read_text().splitlines() if '=' in line)
        secret = fields.get('psk', '')
        if len(secret) != 64 or any(c not in '0123456789abcdefABCDEF' for c in secret):
            raise ValueError('Invalid saved PSK encoding')
        original = decrypt.records
        try:
            decrypt.records = lambda path: prefix(original(path), counts, 'handshake')
            keys = decrypt.handshakes(args.radio, bytes.fromhex(secret), counts)
        finally:
            decrypt.records = original
        if not keys:
            raise ValueError('No authenticated matching handshake')

    beacons = []
    for wall, tsf, packet in prefix(clock.radio_records(args.radio), counts, 'beacon_scan'):
        if (tsf is not None and len(packet) >= 32 and packet[0] == 128
                and packet[10:16] == ap):
            beacons.append((wall, int.from_bytes(packet[24:32], 'little') - tsf))
    beacons.sort()
    beacon_times = [t for t, _ in beacons]
    formats, video = {}, {}
    seen = set()
    radio = prefix(clock.radio_records(args.radio), counts, 'radio')
    frames = prefix(decrypt.records(args.radio), counts, 'frames')
    for (wall, tsf, packet), (sec, us, other, flags) in zip(radio, frames):
        if wall != sec * 1000000 + us or packet != other:
            raise ValueError('Radio readers disagree; refusing timestamp alignment')
        parsed = decrypt.data_frame(packet, flags)
        if not parsed or packet[10:16] != ap or packet[4:10] != pad:
            continue
        plain = None
        if args.decrypted_radio:
            plain = companions.get(packet_key(wall, packet))
        elif packet[1] & 64:
            _, offset, tid, aad = parsed
            body = packet[offset:]
            if len(body) < 16 or not body[3] & 32:
                continue
            nonce = bytes((tid,)) + packet[10:16] + bytes(body[i] for i in (7, 6, 5, 4, 1, 0))
            for (key_ap, station, _), aes in keys.items():
                if (key_ap, station) != (ap, pad):
                    continue
                try:
                    plain = aes.decrypt(nonce, body[8:], aad)
                    break
                except decrypt.InvalidTag:
                    pass
        if plain is None:
            counts['unmatched_or_unauthenticated_ap_observations'] += 1
            continue
        if not plain.startswith(LLC_IP):
            continue
        parsed_ip = udp(plain[8:])
        if not parsed_ip:
            continue
        identity, port, payload = parsed_ip
        if identity in seen:
            continue
        seen.add(identity)
        ip_status, udp_status = checksum_status(plain[8:])
        counts['ipv4_checksum_' + ip_status] += 1
        counts['udp_checksum_' + udp_status] += 1
        if port == 50121 and len(payload) == 32 and payload[0] == 4:
            kind, stamp = 'format', int.from_bytes(payload[8:12], 'little')
        elif port == 50120 and len(payload) >= 16 and payload[2] & 64:
            kind, stamp = 'video_first', int.from_bytes(payload[4:8], 'big')
        else:
            continue
        counts[kind + '_unique'] += 1
        if tsf is None:
            counts[kind + '_no_tsf'] += 1
            continue
        i = bisect.bisect_left(beacon_times, wall)
        near = beacons[max(0, i - 3):i + 4]
        if not near or min(abs(t - wall) for t, _ in near) >= 500000:
            counts[kind + '_no_near_beacon'] += 1
            continue
        offsets = [v for _, v in near]
        if max(offsets) - min(offsets) > 500:
            counts[kind + '_unstable_beacon_clock'] += 1
            continue
        estimated_ap = tsf + round(statistics.median(offsets))
        target = formats if kind == 'format' else video
        target.setdefault(stamp, (wall, signed_delta(estimated_ap, stamp)))
    ages = [age for _, age in formats.values()]
    return {
        'radio': str(args.radio), 'counts': dict(counts), 'beacons': len(beacons),
        'format_age_us': distribution(ages),
        'format_age_at_least_8000us': sum(age >= 8000 for age in ages),
        'format_age_negative': sum(age < 0 for age in ages),
        'video_first_age_us': distribution([age for _, age in video.values()]),
        'format_to_same_timestamp_video_first_us': distribution(
            [video[stamp][0] - value[0] for stamp, value in formats.items() if stamp in video]),
        'limitations': 'First observed radio transmission is not firmware task execution or proof of receipt. '
            'Uses nearby same-AP beacons; rejects local offset spreads above 500us. '
            'AP TSF is a proxy for the synchronized GamePad counter, not a live register read. '
            'Aged-out format messages do not themselves prove a reset. '
            'Repeated payload identities are deduplicated; captures must be shorter than timestamp wrap. '
            'An existing decrypted companion is trusted input, not reauthenticated by this mode.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--radio', required=True, type=Path)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--decrypted-radio', type=Path)
    source.add_argument('--credentials', type=Path)
    parser.add_argument('--ap', default='40:d2:8a:bf:fc:a8')
    parser.add_argument('--pad', default='40:d2:8a:ab:90:00')
    args = parser.parse_args()
    result = analyze(args)
    output = json.dumps(result, indent=2) + '\n'
    print(output, end='')


if __name__ == '__main__':
    main()
