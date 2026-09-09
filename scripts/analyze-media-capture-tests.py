#!/usr/bin/env python3
"""Independent synthetic packets for the passive timing analyzer."""
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("capture", Path(__file__).with_name("analyze-media-capture.py"))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def udp(port, payload, reverse=False):
    ip = bytearray(20)
    ip[0], ip[9] = 0x45, 17
    ip[12:16], ip[16:20] = (bytes([192, 168, 1, 11 if reverse else 10]),
                            bytes([192, 168, 1, 10 if reverse else 11]))
    return bytes(ip) + struct.pack("!HHHH", 50020, port, len(payload) + 8, 0) + payload


def stream(period):
    packets = []
    sequence = 0
    for frame in range(3):
        start = frame * period + 5000
        stamp = (0xfffff000 + frame * period) & 0xffffffff
        fmt = bytearray(32)
        fmt[0] = 4
        fmt[8:12] = stamp.to_bytes(4, "little")
        packets.append((start - 5000, udp(50121, fmt)))
        for chunk in range(5):
            header = bytearray(16)
            header[0], header[1] = 0xf0 | (sequence >> 8), sequence & 255
            header[2], header[3] = 0x28 | (0xc0 if chunk == 0 else 0) | (0x10 if chunk == 4 else 0), 4
            header[4:8] = stamp.to_bytes(4, "big")
            header[8:12] = bytes([0x83, 0x85, 6, 0x80])
            packets.append((start + chunk * 3000, udp(50120, header + b"abcd")))
            sequence += 1
    packets.append((100000, udp(50010, b"\x01\0\0\0", reverse=True)))
    return packets


class TimingTests(unittest.TestCase):
    def analyze(self, packets):
        with patch.object(capture, "records", return_value=packets):
            return capture.analyze("unused.pcap")

    def test_cadence_chunk_completion_and_wrapped_timestamp(self):
        for period in (16683, 33366):
            result = self.analyze(stream(period))
            self.assertAlmostEqual(result["observed_video_fps"], 1e6 / period)
            self.assertEqual(result["counts"]["complete_frames"], 3)
            self.assertEqual(result["counts"]["resync_requests"], 1)
            self.assertNotIn("video_timestamp_regressions", result["counts"])
            self.assertEqual(result["frame_metrics"]["format_to_video_us"]["median"], 5000)
            self.assertEqual(result["frame_metrics"]["span_us"]["max"], 12000)
            self.assertEqual(result["frame_metrics"]["payload_bytes"]["median"], 20)
            for index, chunk in enumerate(result["chunk_metrics"]):
                self.assertEqual(chunk["end_offset_us"]["median"], index * 3000)

    def test_loss_excludes_broken_frame_from_chunk_metrics(self):
        packets = stream(16683)
        del packets[3]
        result = self.analyze(packets)
        self.assertEqual(result["counts"]["invalid_frames"], 1)
        self.assertEqual(result["counts"]["video_sequence_discontinuities"], 1)
        self.assertEqual(result["chunk_metrics"][0]["end_offset_us"]["samples"], 2)

    def test_multi_packet_chunk_span_and_size(self):
        packets = stream(16683)[:6]
        timestamp, packet = packets[1]
        # Split the first chunk into two datagrams, preserving frame start
        # on the first packet and moving chunk end to the second.
        first, second = bytearray(packet), bytearray(packet)
        first[30] &= ~0x20
        second[30] &= ~0x40
        second[29] = 1
        packets[1] = timestamp, bytes(first)
        packets.insert(2, (timestamp + 1000, bytes(second)))
        for index in range(3, len(packets)):
            time, packet = packets[index]
            changed = bytearray(packet)
            changed[29] += 1
            packets[index] = time, bytes(changed)
        result = self.analyze(packets)
        chunk = result["chunk_metrics"][0]
        self.assertEqual(result["counts"]["complete_frames"], 1)
        self.assertEqual(chunk["span_us"]["median"], 1000)
        self.assertEqual(chunk["packets"]["median"], 2)
        self.assertEqual(chunk["payload_bytes"]["median"], 8)


if __name__ == "__main__":
    unittest.main()
