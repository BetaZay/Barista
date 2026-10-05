"""Conservative compressed Block ACK lookup; no decryption or key access.

Layout follows drc-wireshark/epan/dissectors/packet-ieee80211.c:
BA control at 16, starting sequence control at 18, little-endian bitmap at 20.
Ordinary ACKs have no transmitter/sequence identity and are not counted here.
"""
import bisect
from collections import defaultdict


class BlockAckIndex:
    def __init__(self):
        self.times = defaultdict(list)
        self.frames = 0

    def add(self, wall, packet, ap):
        if len(packet) != 28 or packet[0] != 0x94 or packet[4:10] != ap:
            return
        control = int.from_bytes(packet[16:18], 'little')
        # Only single-TID compressed BA, with reserved bits clear.
        if control & 6 != 4 or control & 0x0ff8:
            return
        start = int.from_bytes(packet[18:20], 'little')
        if start & 15:
            return
        start >>= 4
        bitmap = int.from_bytes(packet[20:28], 'little')
        self.frames += 1
        for bit in range(64):
            if bitmap & (1 << bit):
                self.times[bytes(packet[10:16]), control >> 12, (start + bit) % 4096].append(wall)

    def finish(self):
        for times in self.times.values():
            times.sort()

    def acknowledged_at(self, peer, tid, sequence, first_observation):
        times = self.times.get((peer, tid, sequence), ())
        i = bisect.bisect_left(times, first_observation)
        # Prevent old/future uses of a wrapping sequence number from matching.
        if i < len(times) and times[i] - first_observation <= 100000:
            return times[i]
        return None
