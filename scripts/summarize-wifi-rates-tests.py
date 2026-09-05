import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("rates", Path(__file__).with_name("summarize-wifi-rates.py"))
rates = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rates)


def packet(fields, extended=False):
    present = sum(1 << i for i in fields) | (0x80000000 if extended else 0)
    raw = bytearray(struct.pack("<BBHI", 0, 0, 0, present))
    if extended: raw.extend(bytes(4))
    for i, (alignment, size) in enumerate(rates.SIZES):
        if i in fields:
            raw.extend(bytes((-len(raw)) % alignment))
            assert len(fields[i]) == size
            raw.extend(fields[i])
    struct.pack_into("<H", raw, 2, len(raw))
    return bytes(raw) + b"frame"


class RateParsing(unittest.TestCase):
    def test_mcs_with_mac_xchannel(self):
        for index in (4, 5, 6, 7):
            raw = packet({0: bytes(8), 1: b"\0", 3: bytes(4), 5: b"\xce",
                          11: b"\1", 18: bytes(8), 19: bytes((7, 0, index))})
            self.assertEqual(rates.decode(raw), (b"frame", 0, f"HT-MCS{index}-LGI"))

    def test_extended_bitmap_alignment(self):
        self.assertEqual(rates.decode(packet({0: bytes(8), 1: b"\0", 2: b"\x30"}, True)),
                         (b"frame", 0, "legacy-24Mbps"))

    def test_known_bits(self):
        self.assertEqual(rates.decode(packet({19: bytes((2, 0, 6))}))[2], "HT-MCS6-GI-unknown")
        self.assertEqual(rates.decode(packet({19: bytes((7, 4, 6))}))[2], "HT-MCS6-SGI")
        self.assertEqual(rates.decode(packet({19: bytes((0, 0, 6))}))[2], "unknown")

    def test_fcs_flag_and_malformed(self):
        self.assertEqual(rates.decode(packet({1: b"\x40"}))[1], 64)
        for raw in (b"", struct.pack("<BBHI", 0, 0, 100, 0), struct.pack("<BBHI", 0, 0, 8, 1)):
            with self.assertRaises(ValueError): rates.decode(raw)


if __name__ == "__main__":
    unittest.main()
