import unittest
from block_ack import BlockAckIndex


class BlockAckTests(unittest.TestCase):
    def test_bitmap_wrap_peer_tid_and_time(self):
        ap, pad = bytes.fromhex('40d28abffca8'), bytes.fromhex('40d28aab9000')
        p = bytearray(28)
        p[0] = 0x94
        p[4:10], p[10:16] = ap, pad
        p[16:18] = ((5 << 12) | 5).to_bytes(2, 'little')
        p[18:20] = (4095 << 4).to_bytes(2, 'little')
        p[20:28] = (5).to_bytes(8, 'little')
        index = BlockAckIndex()
        index.add(200000, p, ap)
        index.finish()
        self.assertEqual(index.acknowledged_at(pad, 5, 4095, 199000), 200000)
        self.assertEqual(index.acknowledged_at(pad, 5, 1, 199000), 200000)
        self.assertIsNone(index.acknowledged_at(pad, 5, 0, 199000))
        self.assertIsNone(index.acknowledged_at(ap, 5, 4095, 199000))
        self.assertIsNone(index.acknowledged_at(pad, 4, 4095, 199000))
        self.assertIsNone(index.acknowledged_at(pad, 5, 4095, 200001))
        self.assertIsNone(index.acknowledged_at(pad, 5, 4095, 99999))
        p[16] |= 2  # Multi-TID is not supported; must not misparse it.
        index.add(201000, p, ap)
        self.assertEqual(index.frames, 1)


if __name__ == '__main__':
    unittest.main()
