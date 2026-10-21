#!/usr/bin/env python3
"""Authenticate/decrypt a drcd radio capture and match host UDP payloads.

Never prints credentials or decrypted application payloads. Allows only a
truncated final capture record, with a warning; original files stay untouched.
AP timing uses local median beacon offsets, not synchronized PC/Mac wall clocks.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import struct

import decrypt_capture as decrypt
import analyze_clock as clock
from block_ack import BlockAckIndex


def readable_prefix(iterator):
    try:
        yield from iterator
    except ValueError as error:
        if str(error) not in ('Truncated pcap packet', 'Truncated pcap record',
                              'Truncated record', 'Truncated record header'):
            raise


def summary(values):
    v = sorted(values)
    if not v:
        return {'count': 0}
    return dict(count=len(v), min=v[0], median=v[len(v)//2],
                p90=v[len(v)*9//10], p99=v[len(v)*99//100], max=v[-1])


def udp(ip):
    if len(ip) < 28 or ip[0] >> 4 != 4 or ip[9] != 17:
        return None
    h = (ip[0] & 15) * 4
    if h < 20 or len(ip) < h + 8 or int.from_bytes(ip[6:8], 'big') & 0x3fff:
        return None
    sp, dp, n, _ = struct.unpack_from('!HHHH', ip, h)
    if n < 8 or len(ip) < h + n:
        return None
    payload = ip[h+8:h+n]
    identity = hashlib.sha256(ip[12:20] + ip[h:h+4] + payload).digest()
    return identity, dp, payload


def analyze(args):
    original = decrypt.records
    decrypt.records = lambda path: readable_prefix(original(path))
    counts = Counter()
    # Read only the PSK field; no key material enters output or exceptions.
    with open(args.credentials) as stream:
        fields = dict(line.rstrip('\n').split('=', 1) for line in stream if '=' in line)
    secret = fields.get('psk', '')
    if len(secret) != 64 or any(c not in '0123456789abcdefABCDEF' for c in secret):
        raise ValueError('Invalid saved PSK encoding')
    keys = decrypt.handshakes(args.radio, bytes.fromhex(secret), counts)
    if not keys:
        return dict(counts=counts, result='No authenticated matching handshake')
    ap = bytes.fromhex(args.ap.replace(':', ''))
    beacons = []
    acknowledgments = BlockAckIndex()
    for wall, tsf, p in readable_prefix(clock.radio_records(args.radio)):
        acknowledgments.add(wall, p, ap)
        if tsf is not None and len(p) >= 32 and p[0] == 128 and p[10:16] == ap:
            beacons.append((wall, int.from_bytes(p[24:32], 'little') - tsf))
    acknowledgments.finish()
    bt = [b[0] for b in beacons]
    host = {}
    for wall, ip in clock.media.records(args.host):
        parsed = udp(ip)
        if parsed:
            identity, port, payload = parsed
            host[identity] = (wall, port, payload)
    matched = {}
    ages = defaultdict(list)
    frames = {}
    request_times = []
    request_packets = set()
    # Both readers strip FCS and skip explicitly bad-FCS records. Assert their
    # alignment before using radiotap TSF for a decrypted packet.
    air = readable_prefix(clock.radio_records(args.radio))
    last_wall = None
    for (sec, us, p, flags), (wall, tsf, same) in zip(decrypt.records(args.radio), air):
        if wall != sec*1000000+us or same != p:
            raise ValueError('Radio parser alignment mismatch')
        last_wall = wall
        parsed = decrypt.data_frame(p, flags)
        if not parsed or not p[1] & 64:
            continue
        _, offset, tid, aad = parsed
        body = p[offset:]
        if len(body) < 16 or not body[3] & 32:
            continue
        plain = None
        nonce = bytes((tid,)) + p[10:16] + bytes(body[i] for i in (7,6,5,4,1,0))
        for (key_ap, sta, _), aes in keys.items():
            if {p[4:10],p[10:16]} != {key_ap,sta}:
                continue
            try:
                plain = aes.decrypt(nonce, body[8:], aad)
                break
            except decrypt.InvalidTag:
                pass
        if plain is None:
            counts['unauthenticated_encrypted_observations'] += 1
            continue
        counts['authenticated_observations'] += 1
        if not plain.startswith(bytes.fromhex('aaaa030000000800')):
            continue
        parsed = udp(plain[8:])
        if not parsed:
            continue
        identity, port, payload = parsed
        if identity not in host:
            counts['udp_not_matched_to_host'] += 1
            continue
        # Every resync request has the same four-byte UDP payload. Deduplicate
        # radio retransmissions by CCMP packet identity, NOT payload identity,
        # or an entire session's recovery requests collapse to a single event.
        if port == 50010 and payload == bytes((1,0,0,0)):
            radio_identity = (p[10:16], tid, body[:8])
            if radio_identity not in request_packets:
                request_packets.add(radio_identity)
                request_times.append(wall)
                counts['unique_air_recovery_requests'] += 1
        if identity in matched:
            counts['duplicate_matched_observations'] += 1
            # Only video payloads carry unique per-frame timestamps here.
            # Identical control payloads minutes apart are not Wi-Fi retries.
            if port == 50120:
                matched[identity][1] = wall
            continue
        matched[identity] = [wall,wall]
        counts['unique_matched_udp'] += 1
        if port != 50120 or len(payload) < 16:
            continue
        stamp = int.from_bytes(payload[4:8], 'big')
        f = frames.setdefault(stamp, dict(times=[], identities=set(), ack_times={}, idr=128 in payload[8:16]))
        f['times'].append(wall)
        f['identities'].add(identity)
        sequence_control = int.from_bytes(p[22:24], 'little')
        if not sequence_control & 15:
            ack = acknowledgments.acknowledged_at(p[4:10], tid, sequence_control >> 4, wall)
            if ack is not None:
                f['ack_times'][identity] = ack
                counts['unique_video_packets_block_acked'] += 1
        if tsf is not None and beacons:
            i = bisect.bisect_left(bt, wall)
            near = beacons[max(0,i-3):i+4]
            if near and min(abs(wall-t) for t,_ in near) < 500000:
                correction = round(statistics.median(v for _,v in near))
                age = ((tsf+correction-stamp+2**31) % 2**32)-2**31
                kind = 'idr' if f['idr'] else 'p'
                if payload[2] & 64:
                    ages[kind+'_first_packet_age_us'].append(age)
                if payload[2] & 16:
                    ages[kind+'_last_packet_age_us'].append(age)
    # Restrict coverage comparisons to host frames that intersect the Mac's
    # matched video interval; not the longer host capture outside that window.
    host_frames = defaultdict(set)
    for identity, (wall,port,payload) in host.items():
        if port == 50120 and len(payload) >= 16:
            host_frames[int.from_bytes(payload[4:8], 'big')].add(identity)
    complete = incomplete = 0
    complete_idrs = []
    acked_idrs = []
    acked_frames = 0
    for stamp, f in frames.items():
        if f['identities'] == host_frames[stamp]:
            complete += 1
            if f['idr']:
                complete_idrs.append(max(f['times']))
            if set(f['ack_times']) == f['identities']:
                acked_frames += 1
                if f['idr']:
                    acked_idrs.append(max(f['ack_times'].values()))
        else:
            incomplete += 1
    complete_idrs.sort()
    request_times.sort()
    continued = 0
    for t in complete_idrs:
        i = bisect.bisect_left(request_times, t+5000)
        if i < len(request_times) and request_times[i] <= t+50000:
            continued += 1
    acked_continued = 0
    for t in acked_idrs:
        i = bisect.bisect_left(request_times, t+5000)
        if i < len(request_times) and request_times[i] <= t+50000:
            acked_continued += 1
    return dict(counts=counts, beacon_count=len(beacons),
                matched_video_frames_complete=complete, matched_video_frames_incomplete=incomplete,
                complete_idrs_seen=len(complete_idrs),
                complete_idrs_followed_by_request_5_to_50ms=continued,
                block_ack_evidence=dict(control_frames=acknowledgments.frames,
                    video_frames_fully_block_acked=acked_frames,
                    idrs_fully_block_acked=len(acked_idrs),
                    idrs_followed_by_request_5_to_50ms_after_full_ack=acked_continued),
                video_age_on_ap_clock_us={k:summary(v) for k,v in ages.items()},
                matched_repeat_span_us=summary([b-a for a,b in matched.values() if b>a]),
                limitations='Capture prefix only if final record truncated. Missing Mac packets '
                'are not proof of GamePad loss. Block ACK is MAC receipt, not IP delivery or '
                'decoding; absent Block ACK is unknown, not loss (ordinary ACKs are excluded). '
                'A later request may concern a subsequent frame. ACK matching uses peer, TID, '
                'sequence and a 100ms window. Local median beacons estimate AP clock.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--radio', required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--credentials', default='/var/lib/drcd/credentials.conf')
    parser.add_argument('--ap', default='40:d2:8a:bf:fc:a8')
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(args), indent=2))
    except PermissionError:
        parser.exit(1, 'Credential read denied; run with permission to read the saved credential file.\n')
