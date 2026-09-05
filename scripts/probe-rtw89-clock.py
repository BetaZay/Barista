#!/usr/bin/env python3
"""Compare RTL8852BE AP-port TSF registers with received radiotap timestamps.

Run as root alongside an existing drcd capture. Changes only the debugfs
read_reg address selector; NEVER writes hardware registers or clock values.
No credential access. No interface changes. Do not run another register
debugger simultaneously because the selector is shared.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import struct
import time


def radiotap_tsf(packet):
    if len(packet) < 8:
        return None
    size = struct.unpack_from('<H', packet, 2)[0]
    word = present = struct.unpack_from('<I', packet, 4)[0]
    offset = 8
    if not present & 1 or size > len(packet):
        return None
    while word & 0x80000000:
        if offset + 4 > size:
            return None
        word = struct.unpack_from('<I', packet, offset)[0]
        offset += 4
    offset = (offset + 7) // 8 * 8
    if offset + 8 > size:
        return None
    frame = packet[size:]
    # Received traffic from our GamePad, not potentially delayed TX reports.
    if len(frame) < 24 or frame[0] & 12 != 8 or frame[10:16] != bytes.fromhex('40d28aab9000'):
        return None
    return struct.unpack_from('<Q', packet, offset)[0]


def read_register(path, address):
    with path.open('w') as selector:
        selector.write(f'{address:x} 4\n')
    with path.open() as reader:
        result = reader.read()
    match = re.search(r'0x([0-9a-fA-F]+)\s*=\s*0x([0-9a-fA-F]+)', result)
    if not match or int(match[1], 16) != address:
        raise ValueError('Register response mismatch; selector may have another user')
    return int(match[2], 16)


def run(interface, monitor, rounds):
    if not (Path('/sys/class/net') / monitor).exists():
        raise ValueError(f'Monitor {monitor} does not exist. Start the drcd capture, '
                         'turn on the GamePad, and leave it running while executing '
                         'this probe in a second terminal. Do not stop drcd first.')
    device = Path('/sys/class/net') / interface
    if (device / 'device/driver').resolve().name != 'rtw89_8852be':
        raise ValueError('This probe is limited to rtw89_8852be / RTL8852BE')
    phy = (device / 'phy80211').resolve().name
    paths = list((Path('/sys/kernel/debug/ieee80211') / phy).glob('rtw89/**/read_reg'))
    if len(paths) != 1:
        raise ValueError('Expected exactly one rtw89 debugfs read_reg file; check debugfs availability')
    path = paths[0]
    print(json.dumps({'driver': 'rtw89_8852be', 'phy': phy, 'read_selector': str(path),
                      'notice': 'hardware register reads only; debug read selector is shared'}), flush=True)
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
        capture.bind((monitor, 0))
        capture.settimeout(2)
        for _ in range(rounds):
            # Discard queued observations before timing a fresh receive.
            capture.setblocking(False)
            for _ in range(10000):
                try:
                    capture.recv(8192)
                except BlockingIOError:
                    break
            capture.settimeout(2)
            deadline = time.monotonic() + 3
            sample = None
            while time.monotonic() < deadline:
                try:
                    packet = capture.recv(8192)
                except TimeoutError:
                    continue
                observed = time.monotonic_ns()
                sample = radiotap_tsf(packet)
                if sample:
                    break
            if not sample:
                raise ValueError('No GamePad radiotap TSF received; keep drcd and GamePad running')
            for bank in (0, 1):
                for port in range(5):
                    low_address = 0xc438 + bank * 0x2000 + port * 0x40
                    start = time.monotonic_ns()
                    high1 = read_register(path, low_address + 4)
                    low = read_register(path, low_address)
                    high2 = read_register(path, low_address + 4)
                    end = time.monotonic_ns()
                    if high1 != high2:
                        continue  # crossed a low-word wrap; do not combine epochs
                    tsf = (high1 << 32) | low
                    if tsf in (0, 0xffffffffffffffff):
                        continue
                    elapsed_us = ((start + end) // 2 - observed) // 1000
                    # RX descriptor free_run_cnt is 32 bit; compare modulo 2^32.
                    difference = ((tsf - sample - elapsed_us + 2**31) % 2**32) - 2**31
                    print(json.dumps({'bank': bank, 'port': port, 'port_tsf_us': tsf,
                                      'rx_counter_us': sample,
                                      'port_minus_rx_us': difference,
                                      'read_window_us': (end-start)//1000}), flush=True)
            time.sleep(0.2)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface', nargs='?', default='wlan0')
    parser.add_argument('--monitor', default='drcdtsf')
    parser.add_argument('--rounds', type=int, default=3, choices=range(1, 11))
    args = parser.parse_args()
    try:
        run(args.interface, args.monitor, args.rounds)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Probe failed: {error}\n')
