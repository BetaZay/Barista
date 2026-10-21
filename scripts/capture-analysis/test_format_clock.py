from collections import Counter
from pathlib import Path
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import analyze_format_clock as fmt


class FormatClockTests(unittest.TestCase):
    def test_unsigned_freshness_boundary(self):
        self.assertTrue(fmt.fresh_at_counter(10000, 10000))
        self.assertTrue(fmt.fresh_at_counter(17999, 10000))
        self.assertFalse(fmt.fresh_at_counter(18000, 10000))
        self.assertFalse(fmt.fresh_at_counter(9999, 10000))

    def test_timestamp_wrap(self):
        self.assertTrue(fmt.fresh_at_counter(1000, 0xffffff00))
        self.assertFalse(fmt.fresh_at_counter(9000, 0xffffff00))

    def test_checksum_odd_length_and_corruption(self):
        self.assertTrue(fmt.checksum_valid(b'\xff\xff'))
        self.assertTrue(fmt.checksum_valid(b'\x00\xff\xff'))
        self.assertFalse(fmt.checksum_valid(b'\x00\xfe\xff'))

    def test_udp_checksum_validation(self):
        def checksum(data):
            data += b'\0' * (len(data) % 2)
            value = sum(struct.unpack('!' + 'H' * (len(data) // 2), data))
            while value >> 16:
                value = (value & 0xffff) + (value >> 16)
            return (~value & 0xffff).to_bytes(2, 'big')
        ip = bytearray(20)
        ip[0], ip[9] = 0x45, 17
        ip[2:4] = (31).to_bytes(2, 'big')
        ip[12:20] = bytes((192, 168, 1, 10, 192, 168, 1, 11))
        ip[10:12] = checksum(bytes(ip))
        segment = bytearray(struct.pack('!HHHH', 50020, 50120, 11, 0) + b'abc')
        self.assertEqual(fmt.checksum_status(bytes(ip + segment)), ('valid', 'omitted'))
        segment[6:8] = checksum(bytes(ip[12:20]) + b'\0\x11\0\x0b' + segment)
        self.assertEqual(fmt.checksum_status(bytes(ip + segment)), ('valid', 'valid'))
        segment[-1] ^= 1
        self.assertEqual(fmt.checksum_status(bytes(ip + segment)), ('valid', 'invalid'))
        ip[8] ^= 1
        self.assertEqual(fmt.checksum_status(bytes(ip + segment)), ('invalid', 'invalid'))

    def test_truncation_is_explicit_and_other_errors_propagate(self):
        def records(message):
            yield 1
            raise ValueError(message)
        counts = Counter()
        self.assertEqual(list(fmt.prefix(records('Truncated record'), counts, 'radio')), [1])
        self.assertEqual(counts['radio_truncated_tail'], 1)
        with self.assertRaisesRegex(ValueError, 'Bad magic'):
            list(fmt.prefix(records('Bad magic'), counts, 'radio'))

    def test_companion_alignment_and_beacon_clock(self):
        ap = bytes.fromhex('40d28abffca8')
        pad = bytes.fromhex('40d28aab9000')
        beacon = bytearray(32)
        beacon[0] = 128
        beacon[10:16] = ap
        beacon[24:32] = (100000).to_bytes(8, 'little')
        foreign = bytearray(beacon)
        foreign[10] ^= 1
        foreign[24:32] = (900000000).to_bytes(8, 'little')
        format_payload = bytearray(32)
        format_payload[0] = 4
        format_payload[8:12] = (100000).to_bytes(4, 'little')
        video_payload = bytearray(17)
        video_payload[0] = 0xf0
        video_payload[2] = 0x48
        video_payload[3] = 1
        video_payload[4:8] = (100000).to_bytes(4, 'big')

        def make_packet(port, payload, seq):
            ip = bytearray(28)
            ip[0], ip[9] = 0x45, 17
            ip[12:20] = bytes((192, 168, 1, 10, 192, 168, 1, 11))
            struct.pack_into('!HHHH', ip, 20, port - 100, port, len(payload) + 8, 0)
            frame = bytearray(24)
            frame[0], frame[1] = 8, 2
            frame[4:10], frame[10:16], frame[16:22] = pad, ap, ap
            struct.pack_into('<H', frame, 22, seq << 4)
            return bytes(frame) + fmt.LLC_IP + ip + payload

        one = make_packet(50121, format_payload, 1)
        two = make_packet(50120, video_payload, 2)
        air = [(100000, 1000, bytes(beacon)), (100001, 1001, bytes(foreign)),
               (107100, 8100, one), (107110, 8110, two)]
        args = SimpleNamespace(radio=Path('radio'), decrypted_radio=Path('companion'),
                               ap=ap.hex(), pad=pad.hex())

        def raw_records(path):
            return iter((0, wall, packet, 0) for wall, _, packet in air)

        # The analyzer makes two independent scans of the radio capture.
        with patch.object(fmt.clock, 'radio_records', side_effect=lambda _: iter(air)), \
                patch.object(fmt.decrypt, 'records', side_effect=raw_records):
            result = fmt.analyze(args)
        self.assertEqual(result['beacons'], 1)
        self.assertEqual(result['format_age_us']['median'], 7100)
        self.assertEqual(result['video_first_age_us']['median'], 7110)
        self.assertEqual(result['format_to_same_timestamp_video_first_us']['median'], 10)
        self.assertEqual(result['format_age_at_least_8000us'], 0)


if __name__ == '__main__':
    unittest.main()
