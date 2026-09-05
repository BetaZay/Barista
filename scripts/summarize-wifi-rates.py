#!/usr/bin/env python3
"""Summarize AP-to-GamePad radiotap PHY rates/retries without keys or decryption.

Counts observations, not delivered packets. Mac wall clocks are not assumed
synchronized to the PC rate-test timeline. A truncated final record is reported.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct

# Alignment/size through HT MCS. Same standard fields as the local Wireshark
# iterator; Apple's XCHANNEL extension (18) is 4-byte aligned, 8 bytes long.
SIZES = [(8, 8), (1, 1), (1, 1), (2, 4), (2, 2), (1, 1), (1, 1), (2, 2),
         (2, 2), (2, 2), (1, 1), (1, 1), (1, 1), (1, 1), (2, 2), (2, 2),
         (1, 1), (1, 1), (4, 8), (1, 3)]


def decode(raw):
    if len(raw) < 8 or raw[0] != 0:
        raise ValueError("short or unsupported radiotap header")
    length = int.from_bytes(raw[2:4], "little")
    if not 8 <= length <= len(raw):
        raise ValueError("invalid radiotap length")
    present = word = int.from_bytes(raw[4:8], "little")
    offset = 8
    while word & 0x80000000:
        if offset + 4 > length:
            raise ValueError("truncated radiotap presence bitmap")
        word = int.from_bytes(raw[offset:offset + 4], "little")
        offset += 4
    fields = {}
    for index, (alignment, size) in enumerate(SIZES):
        if present & (1 << index):
            offset = (offset + alignment - 1) // alignment * alignment
            if offset + size > length:
                raise ValueError("radiotap field exceeds header")
            fields[index] = raw[offset:offset + size]
            offset += size
    flags = fields.get(1, b"\0")[0]
    rate = "unknown"
    if 19 in fields and fields[19][0] & 2:
        known, options, index = fields[19]
        gi = ("SGI" if options & 4 else "LGI") if known & 4 else "GI-unknown"
        rate = f"HT-MCS{index}-{gi}"
    elif 2 in fields:
        rate = f"legacy-{fields[2][0] / 2:g}Mbps"
    return raw[length:], flags, rate


def packets(path, counts):
    magics = {b"\xd4\xc3\xb2\xa1": ("<", 1), b"\xa1\xb2\xc3\xd4": (">", 1),
              b"\x4d\x3c\xb2\xa1": ("<", 1000), b"\xa1\xb2\x3c\x4d": (">", 1000)}
    with path.open("rb") as stream:
        header = stream.read(24)
        if len(header) != 24 or header[:4] not in magics:
            raise ValueError("expected classic pcap")
        endian, divisor = magics[header[:4]]
        if struct.unpack_from(endian + "I", header, 20)[0] & 0xffff != 127:
            raise ValueError("expected radiotap capture, not companion IP pcap")
        while header := stream.read(16):
            if len(header) != 16:
                counts["truncated_tail"] += 1
                break
            sec, fraction, size, _ = struct.unpack(endian + "IIII", header)
            if size > 16 * 1024 * 1024:
                raise ValueError("oversized capture record")
            raw = stream.read(size)
            if len(raw) != size:
                counts["truncated_tail"] += 1
                break
            yield sec * 1000000 + fraction // divisor, raw


def analyze(path, ap, pad, bin_seconds=5):
    counts = Counter()
    rates = Counter()
    bins = defaultdict(lambda: dict(rates=Counter(), observations=0, retry_observations=0))
    first = last = None
    for wall, raw in packets(path, counts):
        try:
            frame, flags, rate = decode(raw)
        except ValueError:
            counts["unsupported_or_malformed_radiotap"] += 1
            continue
        if flags & 0x40:
            counts["bad_fcs_skipped"] += 1
            continue
        if len(frame) < 24 or frame[0] & 12 != 8 or frame[10:16] != ap or frame[4:10] != pad:
            continue
        if first is None:
            first = wall
        last = wall
        retry = bool(frame[1] & 8)
        counts["ap_to_pad_observations"] += 1
        counts["retry_observations"] += retry
        rates[rate] += 1
        bucket = bins[(wall - first) // (bin_seconds * 1000000) * bin_seconds]
        bucket["rates"][rate] += 1
        bucket["observations"] += 1
        bucket["retry_observations"] += retry
    return dict(capture=str(path), counts=counts, rates=rates,
        first_ap_observation_unix_us=first, last_ap_observation_unix_us=last,
        bins_from_first_ap_observation_s=dict(sorted(bins.items())),
        limitations="Radiotap reports PHY rates, not goodput or successful reception. Retry counts "
        "include repeated observations and are not loss percentages. No packet decryption or "
        "authentication performed. PC/Mac clocks may differ. VHT/HE rates are not decoded.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--ap", default="40:d2:8a:bf:fc:a8")
    parser.add_argument("--pad", default="40:d2:8a:ab:90:00")
    args = parser.parse_args()
    try:
        ap, pad = [bytes.fromhex(value.replace(":", "")) for value in (args.ap, args.pad)]
        if len(ap) != 6 or len(pad) != 6:
            raise ValueError("expected six-byte MAC addresses")
        print(json.dumps(analyze(args.capture, ap, pad), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")
