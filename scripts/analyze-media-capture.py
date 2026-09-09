#!/usr/bin/env python3
"""Check drcd's companion classic-pcap UDP capture without third-party packages."""

import argparse
import collections
import ipaddress
import json
import struct


def records(path):
    with open(path, "rb") as stream:
        header = stream.read(24)
        formats = {b"\xd4\xc3\xb2\xa1": ("<", 1000000),
                   b"\xa1\xb2\xc3\xd4": (">", 1000000),
                   b"\x4d\x3c\xb2\xa1": ("<", 1000000000),
                   b"\xa1\xb2\x3c\x4d": (">", 1000000000)}
        if len(header) != 24 or header[:4] not in formats:
            raise ValueError("expected a classic pcap file produced by tcpdump")
        endian, scale = formats[header[:4]]
        link = struct.unpack_from(endian + "I", header, 20)[0] & 0xffff
        if link not in (1, 101, 113, 276):
            raise ValueError(f"unsupported link type {link}; use the companion -ip.pcap")
        while packet_header := stream.read(16):
            if len(packet_header) != 16:
                raise ValueError("truncated packet header")
            sec, fraction, size, _ = struct.unpack(endian + "IIII", packet_header)
            packet = stream.read(size)
            if len(packet) != size:
                raise ValueError("truncated packet body; stop capture before analysis")
            offset, protocol = 0, 0x0800
            if link == 1:
                offset = 14
                if len(packet) < offset:
                    continue
                protocol = int.from_bytes(packet[12:14], "big")
                while protocol in (0x8100, 0x88a8) and len(packet) >= offset + 4:
                    protocol = int.from_bytes(packet[offset + 2:offset + 4], "big")
                    offset += 4
            elif link == 113:
                offset = 16
                protocol = int.from_bytes(packet[14:16], "big")
            elif link == 276:
                offset = 20
                protocol = int.from_bytes(packet[:2], "big")
            if protocol == 0x0800:
                yield sec * 1000000 + fraction * 1000000 // scale, packet[offset:]


def gaps(values):
    intervals = sorted(b - a for a, b in zip(values, values[1:]))
    if not intervals:
        return {}
    return {"min_us": intervals[0], "median_us": intervals[len(intervals) // 2],
            "max_us": intervals[-1], "below_100us": sum(x < 100 for x in intervals)}


def distribution(values):
    if not values:
        return {}
    ordered = sorted(values)
    return {"samples": len(values), "min": ordered[0],
            "median": ordered[len(ordered) // 2],
            "p95": ordered[min(len(ordered) - 1, (95 * len(ordered) - 1) // 100)],
            "max": ordered[-1]}


def analyze(path):
    counts = collections.Counter()
    times = collections.defaultdict(list)
    previous_seq = {}
    previous_timestamp = None
    frame = None
    formats = {}
    frame_metrics = collections.defaultdict(list)
    chunk_metrics = [collections.defaultdict(list) for _ in range(5)]
    previous_end = None
    for capture_time, ip in records(path):
        if len(ip) < 20 or ip[0] >> 4 != 4 or ip[9] != 17:
            continue
        ihl = (ip[0] & 15) * 4
        if ihl < 20 or len(ip) < ihl + 8:
            counts["truncated_ip"] += 1
            continue
        console = ipaddress.IPv4Address("192.168.1.10").packed
        gamepad = ipaddress.IPv4Address("192.168.1.11").packed
        forward = ip[12:16] == console and ip[16:20] == gamepad
        reverse = ip[12:16] == gamepad and ip[16:20] == console
        if not (forward or reverse):
            continue
        if int.from_bytes(ip[6:8], "big") & 0x3fff:
            counts["fragmented_ip_skipped"] += 1
            continue
        _, port, length, _ = struct.unpack_from("!HHHH", ip, ihl)
        if length < 8 or ihl + length > len(ip):
            counts["truncated_udp"] += 1
            continue
        payload = ip[ihl + 8:ihl + length]
        if reverse:
            if port == 50010 and payload == b"\x01\x00\x00\x00":
                counts["resync_requests"] += 1
                times["resync"].append(capture_time)
            continue
        if port not in (50120, 50121):
            continue
        is_video = port == 50120
        header_size = 16 if is_video else 8
        if len(payload) < header_size:
            counts["short_media_header"] += 1
            continue
        kind = "video" if is_video else ("format" if payload[0] & 4 else "pcm")
        counts[kind] += 1
        times[kind].append(capture_time)
        declared = ((payload[2] & 7) << 8 | payload[3]) if is_video else \
            int.from_bytes(payload[2:4], "big")
        # Real console format messages declare zero with a 24-byte body.
        reference_format_length = kind == "format" and declared == 0 and len(payload) == 32
        if declared != len(payload) - header_size and not reference_format_length:
            counts["payload_length_errors"] += 1
        if kind == "format":
            if len(payload) == 32:
                formats[int.from_bytes(payload[8:12], "little")] = capture_time
                if len(formats) > 512:
                    del formats[next(iter(formats))]
            continue
        seq = ((payload[0] & 3) << 8) | payload[1]
        if kind in previous_seq and seq != (previous_seq[kind] + 1) % 1024:
            counts[kind + "_sequence_discontinuities"] += 1
            if frame is not None and is_video:
                frame["broken"] = True
        previous_seq[kind] = seq
        if not is_video:
            continue
        timestamp = int.from_bytes(payload[4:8], "big")
        if payload[2] & 0x40:
            times["frame"].append(capture_time)
            if frame is not None:
                counts["unfinished_frames"] += 1
            if previous_timestamp is not None:
                delta = (timestamp - previous_timestamp) & 0xffffffff
                if delta >= 0x80000000:
                    counts["video_timestamp_regressions"] += 1
            previous_timestamp = timestamp
            frame = {"timestamp": timestamp, "chunks": 0, "broken": False,
                     "start": capture_time, "packets": 0, "bytes": 0,
                     "chunk_start": capture_time, "chunk_packets": 0,
                     "chunk_bytes": 0, "completed_chunks": []}
            if timestamp in formats:
                frame_metrics["format_to_video_us"].append(capture_time - formats[timestamp])
            else:
                counts["frames_without_prior_matching_format"] += 1
            if previous_end is not None:
                frame_metrics["previous_end_to_begin_us"].append(capture_time - previous_end)
            counts["idr_frames" if 0x80 in payload[8:16] else "predicted_frames"] += 1
            counts["init_flagged_frames"] += bool(payload[2] & 0x80)
            counts["frame_begins"] += 1
        if frame is None:
            counts["video_packets_outside_frame"] += 1
            continue
        if timestamp != frame["timestamp"]:
            frame["broken"] = True
            counts["inconsistent_frame_timestamps"] += 1
        if frame["chunk_packets"] == 0:
            frame["chunk_start"] = capture_time
        frame["packets"] += 1
        frame["bytes"] += len(payload) - 16
        frame["chunk_packets"] += 1
        frame["chunk_bytes"] += len(payload) - 16
        if payload[2] & 0x20:
            frame["completed_chunks"].append({
                "start_offset_us": frame["chunk_start"] - frame["start"],
                "end_offset_us": capture_time - frame["start"],
                "span_us": capture_time - frame["chunk_start"],
                "packets": frame["chunk_packets"], "payload_bytes": frame["chunk_bytes"]})
            frame["chunks"] += 1
            frame["chunk_packets"] = frame["chunk_bytes"] = 0
        if payload[2] & 0x10:
            valid = frame["chunks"] == 5 and not frame["broken"] and bool(payload[2] & 0x20)
            counts["complete_frames" if valid else "invalid_frames"] += 1
            if valid:
                frame_metrics["span_us"].append(capture_time - frame["start"])
                frame_metrics["packets"].append(frame["packets"])
                frame_metrics["payload_bytes"].append(frame["bytes"])
                for index, chunk in enumerate(frame["completed_chunks"]):
                    for key, value in chunk.items():
                        chunk_metrics[index][key].append(value)
            previous_end = capture_time
            frame = None
    if frame is not None:
        counts["unfinished_frames"] += 1
    frame_times = times["frame"]
    fps = ((len(frame_times) - 1) * 1000000 / (frame_times[-1] - frame_times[0])
           if len(frame_times) > 1 and frame_times[-1] > frame_times[0] else None)
    return {"counts": dict(counts), "observed_video_fps": fps, "host_packet_spacing":
            {kind: gaps(values) for kind, values in times.items()},
            "frame_metrics": {key: distribution(values) for key, values in frame_metrics.items()},
            "chunk_metrics": [{key: distribution(values) for key, values in chunk.items()}
                              for chunk in chunk_metrics],
            "note": "Host capture shows network-stack traffic, not confirmed over-air delivery or decoding. Boundary frames may be partial."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcap")
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(args.pcap), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
