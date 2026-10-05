import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare-real-replay.py'))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)
spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run-replay-experiment.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def ip(port, data):
    header = bytearray(20)
    header[0] = 0x45
    header[9] = 17
    header[12:20] = bytes([192, 168, 1, 10, 192, 168, 1, 11])
    return bytes(header) + struct.pack('!HHHH', port, port, len(data) + 8, 0) + data


def capture(missing=(), idrs=(0,), count=6, format_only=False):
    records = []
    sequence = 0
    for index in range(count):
        wall = 100000 + index * 16683
        stamp = 900000 + index * 16683
        if index not in missing:
            data = bytearray(32)
            data[0] = 4
            data[8:12] = stamp.to_bytes(4, 'little')
            records.append((wall - 5000, ip(50121, data)))
        if format_only and index == 1:
            data = bytearray(32)
            data[0] = 4
            data[8:12] = (stamp - 8000).to_bytes(4, 'little')
            records.append((wall - 7000, ip(50121, data)))
        for chunk in range(5):
            data = bytearray(17)
            data[0] = (sequence >> 8) & 3
            data[1] = sequence & 255
            data[2] = 32 | (64 if chunk == 0 else 0) | (16 if chunk == 4 else 0)
            data[3] = 1
            data[4:8] = stamp.to_bytes(4, 'big')
            data[8] = 128 if index in idrs else 130
            records.append((wall + chunk * 100, ip(50120, data)))
            sequence = (sequence + 1) & 1023
    return sorted(records)


class Preparation(unittest.TestCase):
    def prepare(self, records, seconds=30):
        with patch.object(p.media, 'records', return_value=records):
            return p.prepare('mock.pcap', seconds)

    def test_missing_format_requires_new_idr(self):
        artifact, report = self.prepare(capture(missing=(1,), idrs=(0, 3)))
        self.assertEqual(report['frames'], 3)
        # Cannot skip the missing frame and resume with its dependent P frames.
        self.assertEqual(struct.unpack_from('<I', artifact, 16)[0], 900000 + 3 * 16683)

    def test_format_only_slots_retained(self):
        _, report = self.prepare(capture(format_only=True))
        self.assertEqual(report['frames'], 6)
        self.assertEqual(report['format_packets'], 7)

    def test_limit_ends_on_complete_frame(self):
        _, report = self.prepare(capture(), 0.025)
        self.assertEqual(report['frames'], 2)
        self.assertLess(report['duration_s'], 0.025)

    def test_no_bootstrap_format_rejected(self):
        with self.assertRaisesRegex(ValueError, 'matching preceding format'):
            self.prepare(capture(missing=(0,)))

    def test_duration_bounds(self):
        for seconds in (0, -1, 31):
            with self.assertRaises(ValueError):
                self.prepare(capture(), seconds)

    def test_replay_command_clears_apphook_and_bounds_engine(self):
        command = runner.engine_command('original', Path('/tmp/source.drcrep'), Path('/tmp/control'), 60)
        self.assertIn('420s', command)
        self.assertIn('--kill-after=15s', command)
        self.assertIn('-i', command)
        self.assertIn('DRCD_REAL_REPLAY=/tmp/source.drcrep', command)
        self.assertIn('--black', command)
        self.assertFalse(any('BARISTA_MUG_SOCKET' in part for part in command))

    def test_live_command_uses_normal_file_pipeline(self):
        command = runner.engine_command('live', Path('/tmp/source.mkv'), Path('/tmp/control'), 60)
        self.assertEqual(command[-2:], ['--play', '/tmp/source.mkv'])
        self.assertFalse(any('DRCD_REAL_REPLAY' in part for part in command))


if __name__ == '__main__':
    unittest.main()
