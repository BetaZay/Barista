#!/usr/bin/env python3
"""Authenticate Wii U WPA handshakes and decrypt unicast CCMP, never print keys.

Requires cryptography. Outputs private classic pcap files, refusing overwrite.
Nintendo PTK rotation follows ../drc-wireshark/epan/crypt/airpdcap.c.
"""
import argparse
from collections import Counter
import hashlib
import hmac
import json
import os
import struct

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM


def crc_ok(data, start, size):
    crc = 0xffff
    for byte in data[start:start + size]:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc == int.from_bytes(data[start + size:start + size + 2], 'little')


def read_pmk(path):
    with open(path, 'rb') as stream:
        data = stream.read()
    if len(data) != 0x300 or not crc_ok(data, 1, 0x6a):
        raise ValueError('Invalid EEPROM size or Wi-Fi CRC')
    for offset, value in ((0xfd, 3), (0x1fd, 5), (0x2fd, 4)):
        if data[offset] != value or not crc_ok(data, offset, 1):
            raise ValueError('Unexpected EEPROM layout')
    size = data[0x65]
    if data[0x21] > 32 or not 8 <= size <= 64:
        raise ValueError('Invalid credential lengths')
    secret = data[0x25:0x25 + size]
    if size == 64:
        # Validate without including credential bytes in exception messages.
        if any(c not in b'0123456789abcdefABCDEF' for c in secret):
            raise ValueError('64-byte key is not a hexadecimal PSK')
        return bytes.fromhex(secret.decode('ascii'))
    return hashlib.pbkdf2_hmac('sha1', secret, data[1:1 + data[0x21]], 4096, 32)


def records(path):
    formats = {b'\xd4\xc3\xb2\xa1': ('<', 1000000),
               b'\xa1\xb2\xc3\xd4': ('>', 1000000),
               b'\x4d\x3c\xb2\xa1': ('<', 1000000000),
               b'\xa1\xb2\x3c\x4d': ('>', 1000000000)}
    with open(path, 'rb') as stream:
        header = stream.read(24)
        if len(header) != 24 or header[:4] not in formats:
            raise ValueError('Expected classic pcap')
        endian, scale = formats[header[:4]]
        link = struct.unpack_from(endian + 'I', header, 20)[0] & 0xffff
        if link not in (105, 127):
            raise ValueError('Expected 802.11 or radiotap capture')
        while ph := stream.read(16):
            if len(ph) != 16:
                raise ValueError('Truncated pcap record')
            sec, frac, size, _ = struct.unpack(endian + 'IIII', ph)
            if size > 16 * 1024 * 1024:
                raise ValueError('Unreasonable capture record size')
            packet = stream.read(size)
            if len(packet) != size:
                raise ValueError('Truncated pcap packet')
            flags = 0
            if link == 127:
                if len(packet) < 8:
                    continue
                rtlen = struct.unpack_from('<H', packet, 2)[0]
                if rtlen < 8 or rtlen > len(packet):
                    continue
                present = struct.unpack_from('<I', packet, 4)[0]
                word, offset = present, 8
                while word & 0x80000000:
                    if offset + 4 > rtlen:
                        raise ValueError('Invalid radiotap bitmap')
                    word = struct.unpack_from('<I', packet, offset)[0]
                    offset += 4
                if present & 1:
                    offset = (offset + 7) // 8 * 8 + 8
                if present & 2:
                    if offset >= rtlen:
                        raise ValueError('Invalid radiotap flags')
                    flags = packet[offset]
                packet = packet[rtlen:]
                if flags & 0x10:
                    packet = packet[:-4]
            if flags & 0x40:  # Explicitly reported bad FCS.
                continue
            yield sec, frac * 1000000 // scale, packet, flags


def data_frame(packet, flags):
    if len(packet) < 24 or packet[0] & 0x0c != 8:
        return None
    four = packet[1] & 3 == 3
    qos = bool(packet[0] & 0x80)
    qoffset = 30 if four else 24
    header_size = qoffset + (2 if qos else 0)
    if qos and packet[1] & 0x80:
        header_size += 4
    body_offset = (header_size + 3) // 4 * 4 if flags & 0x20 else header_size
    if len(packet) < body_offset:
        return None
    priority = packet[qoffset] & 15 if qos else 0
    aad = bytes((packet[0] & 0x8f, packet[1] & 0xc7))
    aad += packet[4:22] + bytes((packet[22] & 15, 0))
    if four:
        aad += packet[24:30]
    if qos:
        aad += bytes((priority, 0))
    return header_size, body_offset, priority, aad


def derive(pmk, ap, sta, anonce, snonce, rotated):
    context = min(ap, sta) + max(ap, sta) + min(anonce, snonce) + max(anonce, snonce)
    ptk = b''.join(hmac.digest(pmk, b'Pairwise key expansion\0' + context + bytes((i,)),
                              'sha1') for i in range(3))[:48]
    return ptk[3:] + ptk[:3] if rotated else ptk


def handshakes(path, pmk, counts):
    messages = []
    for _, _, packet, flags in records(path):
        parsed = data_frame(packet, flags)
        if not parsed or packet[1] & 0x40:
            continue
        body = packet[parsed[1]:]
        if not body.startswith(b'\xaa\xaa\x03\x00\x00\x00\x88\x8e'):
            continue
        eap = body[8:]
        if len(eap) < 99 or eap[1] != 3:
            continue
        size = 4 + int.from_bytes(eap[2:4], 'big')
        if size > len(eap) or size < 99:
            continue
        eap = eap[:size]
        info = int.from_bytes(eap[5:7], 'big')
        if not info & 8:  # pairwise only
            continue
        ds = packet[1] & 3
        if ds not in (1, 2):
            continue
        ap, sta = (packet[4:10], packet[10:16]) if ds == 1 else (packet[10:16], packet[4:10])
        messages.append((ap, sta, info, eap[17:49], eap))
    counts['eapol_key_packets'] = len(messages)
    keys = {}
    for ap, sta, info, snonce, eap in messages:
        if info & 0x80 or not info & 0x100 or not any(snonce) or info & 7 != 2:
            continue
        for other_ap, other_sta, other_info, anonce, _ in messages:
            if (ap, sta) != (other_ap, other_sta) or not other_info & 0x80 or not any(anonce):
                continue
            for rotated in (True, False):
                ptk = derive(pmk, ap, sta, anonce, snonce, rotated)
                expected = hmac.digest(ptk[:16], eap[:81] + bytes(16) + eap[97:], 'sha1')[:16]
                if hmac.compare_digest(expected, eap[81:97]):
                    identity = (ap, sta, ptk[32:48])
                    if identity not in keys:
                        counts['verified_rotated_ptk' if rotated else 'verified_standard_ptk'] += 1
                        keys[identity] = AESCCM(ptk[32:48], tag_length=8)
    return keys


def output_file(path, link):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    stream = os.fdopen(fd, 'wb')
    stream.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 262144, link))
    return stream


def write_packet(stream, sec, usec, packet):
    stream.write(struct.pack('<IIII', sec, usec, len(packet), len(packet)))
    stream.write(packet)


def decrypt_with_pmk(args, pmk):
    counts = Counter()
    keys = handshakes(args.capture, pmk, counts)
    if not keys:
        print(json.dumps(dict(counts, result='No matching authenticated handshake; no output written'), indent=2))
        return 2
    print('Handshake MIC verified. Decrypting with authenticated session keys.', flush=True)
    flows = Counter()
    with output_file(args.output, 105) as out, output_file(args.ip_output, 101) as ipout:
        for sec, usec, packet, flags in records(args.capture):
            parsed = data_frame(packet, flags)
            if not parsed or not packet[1] & 0x40:
                continue
            header_size, offset, priority, aad = parsed
            candidates = [aes for (ap, sta, _), aes in keys.items()
                          if {packet[4:10], packet[10:16]} == {ap, sta}]
            if not candidates:
                continue
            counts['encrypted_unicast_packets'] += 1
            body = packet[offset:]
            if len(body) < 16 or not body[3] & 0x20:
                counts['short_or_not_ccmp'] += 1
                continue
            nonce = bytes((priority,)) + packet[10:16] + bytes(body[i] for i in (7, 6, 5, 4, 1, 0))
            plain = None
            for aes in candidates:
                try:
                    plain = aes.decrypt(nonce, body[8:], aad)
                    break
                except InvalidTag:
                    pass
            if plain is None:
                counts['ccmp_authentication_failed'] += 1
                continue
            counts['ccmp_authenticated'] += 1
            header = bytearray(packet[:header_size])
            header[1] &= ~0x40
            write_packet(out, sec, usec, header + plain)
            if plain.startswith(b'\xaa\xaa\x03\x00\x00\x00\x08\x00'):
                ip = plain[8:]
                write_packet(ipout, sec, usec, ip)
                counts['ipv4_packets'] += 1
                if len(ip) >= 20 and ip[9] == 17:
                    ihl = (ip[0] & 15) * 4
                    if ihl >= 20 and len(ip) >= ihl + 8:
                        sport, dport = struct.unpack_from('!HH', ip, ihl)
                        flows[f'{sport}->{dport}'] += 1
    print(json.dumps({'counts': dict(counts), 'udp_ports': dict(flows)}, indent=2))
    return 0


def run(args):
    return decrypt_with_pmk(args, read_pmk(args.eeprom))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--eeprom', required=True)
    parser.add_argument('--capture', required=True)
    parser.add_argument('--output', required=True, help='Decrypted 802.11 pcap, must not exist')
    parser.add_argument('--ip-output', required=True, help='Decrypted IPv4 pcap, must not exist')
    try:
        raise SystemExit(run(parser.parse_args()))
    except (ValueError, OSError) as error:
        parser.exit(1, f'Error: {error}\n')
