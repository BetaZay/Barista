"""Synthetic delivery analysis fixtures; no real credentials or radio required."""
import io
import struct
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import analyze_delivery as analysis


class DeliveryTests(unittest.TestCase):
    def test_identical_requests_are_distinct_but_radio_retries_are_not(self):
        ap = bytes.fromhex('40d28abffca8')
        pad = bytes.fromhex('40d28aab9000')

        def ip(payload, port):
            packet = bytearray(28) + payload
            packet[0], packet[9] = 0x45, 17
            packet[12:20] = bytes((192,168,1,10,192,168,1,11))
            packet[20:28] = struct.pack('!HHHH', 50110, port, len(payload)+8, 0)
            return bytes(packet)

        video = bytearray(20)
        video[0], video[2], video[3], video[8] = 240, 0x78, 4, 128
        video[4:8] = (100000).to_bytes(4, 'big')
        video_ip = ip(video, 50120)
        request_ip = ip(bytes((1,0,0,0)), 50010)

        def radio(pn):
            packet = bytearray(24)
            packet[0], packet[1] = 8, 66
            packet[4:10], packet[10:16] = pad, ap
            return bytes(packet) + bytes((pn,0,0,32,0,0,0,0)) + bytes(8)

        beacon = bytearray(32)
        beacon[0] = 128
        beacon[10:16] = ap
        beacon[24:32] = (100000).to_bytes(8, 'little')
        ack = bytearray(28)
        ack[0], ack[16], ack[20] = 0x94, 5, 1
        ack[4:10], ack[10:16] = ap, pad
        records = [(1000000, bytes(beacon)), (1006250, radio(1)), (1010000, bytes(ack)),
                   (1020000, radio(2)), (1021000, radio(2)), (1040000, radio(3))]

        class AES:
            def decrypt(self, nonce, body, aad):
                return bytes.fromhex('aaaa030000000800') + (video_ip if nonce[-1] == 1 else request_ip)

        args = SimpleNamespace(credentials='synthetic', radio='synthetic', host='synthetic',
                               ap='40:d2:8a:bf:fc:a8')
        with patch('builtins.open', return_value=io.StringIO('psk='+'0'*64)), \
                patch.object(analysis.decrypt, 'handshakes', return_value={(ap,pad,b''):AES()}), \
                patch.object(analysis.decrypt, 'records', side_effect=lambda _: iter(
                    [(t//1000000,t%1000000,p,0) for t,p in records])), \
                patch.object(analysis.clock, 'radio_records', side_effect=lambda _: iter(
                    [(t,t-900000,p) for t,p in records])), \
                patch.object(analysis.clock.media, 'records', return_value=iter(
                    [(1005000,video_ip),(1019000,request_ip),(1039000,request_ip)])):
            result = analysis.analyze(args)
        self.assertEqual(result['matched_video_frames_complete'], 1)
        self.assertEqual(result['counts']['unique_air_recovery_requests'], 2)
        self.assertEqual(result['complete_idrs_followed_by_request_5_to_50ms'], 1)
        self.assertEqual(result['block_ack_evidence']['idrs_fully_block_acked'], 1)
        self.assertEqual(result['block_ack_evidence']['idrs_followed_by_request_5_to_50ms_after_full_ack'], 1)
        self.assertEqual(result['matched_repeat_span_us']['count'], 0)
        self.assertEqual(result['video_age_on_ap_clock_us']['idr_first_packet_age_us']['median'], 6250)


if __name__ == '__main__':
    unittest.main()
