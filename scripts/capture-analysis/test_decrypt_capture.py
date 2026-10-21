from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

import decrypt_capture as decoder


class DecryptionTests(unittest.TestCase):
    def test_rotation(self):
        args = (bytes(range(32)), bytes(range(6)), bytes(range(6, 12)),
                bytes(range(32)), bytes(range(32, 64)))
        normal = decoder.derive(*args, False)
        rotated = decoder.derive(*args, True)
        self.assertEqual(len(normal), 48)
        self.assertEqual(rotated, normal[3:] + normal[:3])
        # Peer/nonce ordering must not change derivation.
        self.assertEqual(rotated, decoder.derive(args[0], args[2], args[1],
                                                 args[4], args[3], True))

    def test_handshake_authentication(self):
        pmk = bytes(range(32))
        ap, sta = bytes(range(6)), bytes(range(6, 12))
        anonce, snonce = bytes(range(32)), bytes(range(32, 64))
        ptk = decoder.derive(pmk, ap, sta, anonce, snonce, True)

        def frame(info, nonce, from_ap):
            eap = bytearray(99)
            eap[:5] = b'\x02\x03\x00\x5f\x02'
            eap[5:7] = info.to_bytes(2, 'big')
            eap[17:49] = nonce
            if info & 0x100:
                eap[81:97] = decoder.hmac.digest(ptk[:16], eap, 'sha1')[:16]
            header = bytes((8, 2 if from_ap else 1, 0, 0))
            header += (sta + ap if from_ap else ap + sta) + ap + bytes(2)
            return (0, 0, header + b'\xaa\xaa\x03\0\0\0\x88\x8e' + eap, 0)

        packets = [frame(0x8a, anonce, True), frame(0x10a, snonce, False)]
        with patch.object(decoder, 'records', return_value=iter(packets)):
            self.assertEqual(len(decoder.handshakes('', pmk, decoder.Counter())), 1)
        with patch.object(decoder, 'records', return_value=iter(packets)):
            self.assertEqual(len(decoder.handshakes('', bytes(32), decoder.Counter())), 0)

    def test_qos_aad_and_padding(self):
        header = bytes.fromhex('88420000000102030405060708090a0b0c0d0e0f101125001500')
        parsed = decoder.data_frame(header + bytes(20), 0x20)
        self.assertEqual(parsed[:3], (26, 28, 5))
        self.assertEqual(parsed[3], bytes.fromhex('8842000102030405060708090a0b0c0d0e0f101105000500'))

    def test_ccmp_tamper_rejected(self):
        aes = AESCCM(bytes(16), tag_length=8)
        nonce, aad = bytes(13), b'header'
        ciphertext = aes.encrypt(nonce, b'test packet', aad)
        self.assertEqual(aes.decrypt(nonce, ciphertext, aad), b'test packet')
        with self.assertRaises(InvalidTag):
            aes.decrypt(nonce, ciphertext[:-1] + bytes((ciphertext[-1] ^ 1,)), aad)

    def test_output_private_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'output.pcap'
            with decoder.output_file(path, 101) as stream:
                decoder.write_packet(stream, 1, 2, b'payload')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            original = path.read_bytes()
            with self.assertRaises(FileExistsError):
                decoder.output_file(path, 101)
            self.assertEqual(path.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
