import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('reencode', Path(__file__).with_name('reencode-real-replay.py'))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


class Packetization(unittest.TestCase):
    def test_isolated_encoder_override(self):
        with patch.dict(r.os.environ, {'DRCD_INTRA_REFRESH':'0', 'DRCD_VIDEO_QP':'36', 'DRCD_FAST_ENCODE':'1'}):
            baseline = r.encoder_settings()
            experiment = r.encoder_settings(False)
        self.assertEqual(baseline['DRCD_INTRA_REFRESH'], '1')
        self.assertEqual(experiment['DRCD_INTRA_REFRESH'], '0')
        self.assertEqual(baseline['DRCD_VIDEO_QP'], '32')
        self.assertEqual(baseline['DRCD_FAST_ENCODE'], '0')
        self.assertEqual([k for k in baseline if baseline[k] != experiment[k]], ['DRCD_INTRA_REFRESH'])

    def original(self, count):
        header = bytes.fromhex('f000e801123456788082008385060000')
        return [(100+i*100, 0, header+b'x') for i in range(count)]

    def test_preserve_slots_and_bytes(self):
        data = bytes(range(256))*10
        packets, seq = r.packetize(data, self.original(3), 0, 1023)
        self.assertEqual([p[0] for p in packets], [100,200,300])
        self.assertEqual(b''.join(p[2][16:] for p in packets), data)
        self.assertEqual(seq, 2)
        for i, (_, _, p) in enumerate(packets):
            self.assertEqual(((p[0]&3)<<8)|p[1], (1023+i)%1024)
            self.assertEqual(((p[2]&7)<<8)|p[3], len(p)-16)
            self.assertEqual(p[4:16], self.original(3)[0][2][4:16])
            self.assertEqual(bool(p[2]&64), i==0)
            self.assertEqual(bool(p[2]&32), i==2)
            self.assertFalse(p[2]&16)

    def test_expansion_stays_within_chunk_window(self):
        packets, _ = r.packetize(b'x'*7000, self.original(2), 4, 0)
        self.assertEqual(len(packets), 5)
        self.assertEqual(packets[0][0], 100)
        self.assertEqual(packets[-1][0], 200)
        self.assertTrue(all(0 < len(p[2])-16 <= 1694 for p in packets))
        self.assertEqual(sum(bool(p[2][2]&16) for p in packets), 1)
        self.assertTrue(packets[-1][2][2]&16)

    def test_small_chunk(self):
        packets, _ = r.packetize(b'x', self.original(2), 0, 0)
        self.assertEqual(len(packets), 1)
        self.assertTrue(packets[0][2][2]&64)
        self.assertTrue(packets[0][2][2]&32)


if __name__ == '__main__': unittest.main()
