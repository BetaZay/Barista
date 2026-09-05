#!/usr/bin/env python3
"""Follow recreated monitor interfaces; concatenate complete radiotap records safely."""
import argparse
import json
from pathlib import Path
import selectors
import signal
import struct
import subprocess
import sys
import time


class PcapSink:
    def __init__(self, output):
        self.output = output
        self.output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 262144, 127))
        self.output.flush()
        self.buffer = bytearray()
        self.format = None
        self.packets = 0
        self.partial_segments = 0
        self.first_us = self.last_us = None

    def feed(self, data):
        self.buffer.extend(data)
        if self.format is None:
            if len(self.buffer) < 24:
                return
            formats = {b'\xd4\xc3\xb2\xa1': ('<', 1), b'\xa1\xb2\xc3\xd4': ('>', 1),
                       b'\x4d\x3c\xb2\xa1': ('<', 1000), b'\xa1\xb2\x3c\x4d': ('>', 1000)}
            self.format = formats.get(bytes(self.buffer[:4]))
            if self.format is None:
                raise ValueError('tcpdump did not produce classic pcap')
            endian, _ = self.format
            if struct.unpack_from(endian+'I', self.buffer, 20)[0] & 0xffff != 127:
                raise ValueError('tcpdump interface is not radiotap')
            del self.buffer[:24]
        endian, divisor = self.format
        while len(self.buffer) >= 16:
            seconds, fraction, captured, original = struct.unpack_from(endian+'IIII', self.buffer)
            if captured > 262144 or original < captured or fraction >= 1000000*divisor:
                raise ValueError('invalid pcap packet bounds')
            if len(self.buffer) < 16+captured:
                break
            fraction //= divisor
            self.output.write(struct.pack('<IIII', seconds, fraction, captured, original))
            self.output.write(self.buffer[16:16+captured])
            del self.buffer[:16+captured]
            self.packets += 1
            stamp = seconds*1000000+fraction
            if self.first_us is None: self.first_us = stamp
            self.last_us = stamp
        self.output.flush()

    def end_segment(self):
        partial = bool(self.buffer)
        self.partial_segments += partial
        self.buffer.clear()
        self.format = None
        return partial


class RadioFollower:
    def __init__(self, interface, sink, timeout=30, sysfs=Path('/sys/class/net'), tcpdump='tcpdump'):
        self.interface = interface
        self.sink = sink
        self.timeout = timeout
        self.sysfs = sysfs
        self.tcpdump = tcpdump
        self.process = None
        self.identity = None
        self.selector = selectors.DefaultSelector()
        self.events = []
        self.missing_since = None
        self.failures = 0
        self.discard = False

    def event(self, action, **details):
        self.events.append({'time': time.time(), 'event': action, **details})
        print(f'capture-radio: {action} {details}', file=sys.stderr, flush=True)

    def interface_id(self):
        try:
            return int((self.sysfs/self.interface/'ifindex').read_text().strip())
        except (FileNotFoundError, ValueError):
            return None

    def drain(self, timeout=0):
        for key, _ in self.selector.select(timeout):
            data = key.fileobj.read1(65536)
            if data:
                if not self.discard: self.sink.feed(data)
            else:
                self.selector.unregister(key.fileobj)

    def stop_child(self, reason):
        process = self.process
        if process is None: return
        if process.poll() is None: process.send_signal(signal.SIGINT)
        deadline = time.monotonic()+3
        while process.poll() is None and time.monotonic() < deadline:
            self.drain(0.05)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=3)
        while self.selector.get_map(): self.drain(0.05)
        process.stdout.close()
        if self.sink.end_segment(): self.event('partial-record-discarded')
        self.event('detached', ifindex=self.identity, reason=reason, exit_code=process.returncode)
        self.process = None
        self.identity = None

    def step(self):
        identity = self.interface_id()
        if self.process is not None:
            self.drain()
            if self.process.poll() is not None or identity != self.identity:
                failed = identity == self.identity and self.process.poll() is not None
                self.stop_child('tcpdump exited' if failed else 'interface removed/recreated')
                self.failures = self.failures+1 if failed else 0
                if self.failures >= 3:
                    raise RuntimeError('tcpdump failed three times on the same interface')
        if identity is None:
            if self.missing_since is None:
                self.missing_since = time.monotonic()
                self.event('waiting-for-monitor', interface=self.interface)
            if time.monotonic()-self.missing_since > self.timeout:
                raise RuntimeError(f'monitor absent for {self.timeout}s')
        elif self.process is None:
            self.missing_since = None
            self.process = subprocess.Popen([self.tcpdump, '-q', '-U', '-s', '0', '-i',
                self.interface, '-w', '-'], stdout=subprocess.PIPE)
            self.identity = identity
            self.selector.register(self.process.stdout, selectors.EVENT_READ)
            self.event('attached', interface=self.interface, ifindex=identity)
        if self.process is not None: self.drain(0.1)

    def close(self):
        try: self.stop_child('shutdown')
        finally: self.selector.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if '/' in args.interface or args.interface in ('', '.', '..'):
        parser.error('invalid interface name')
    summary = Path(str(args.output)+'.radio.json')
    if summary.exists(): parser.error('radio summary already exists')
    stopping = False
    def stop(signum, frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    status = 0
    # The parent runs as root. Opening this output ourselves means tcpdump may
    # still drop privileges while writing its stdout pipe.
    with args.output.open('xb') as output:
        sink = PcapSink(output)
        follower = RadioFollower(args.interface, sink)
        try:
            while not stopping:
                follower.step()
                if follower.process is None: time.sleep(0.1)
        except (OSError, ValueError, RuntimeError) as error:
            follower.event('fatal', reason=str(error))
            follower.discard = True
            status = 1
        finally:
            follower.close()
            report = {'packets':sink.packets, 'first_packet_us':sink.first_us,
                      'last_packet_us':sink.last_us, 'partial_segments':sink.partial_segments,
                      'exit_status':status, 'events':follower.events,
                      'note':'Interface changes can leave capture gaps; this is not lossless coverage.'}
            with summary.open('x') as stream: json.dump(report, stream, indent=2)
            print(f'capture-radio: packets={sink.packets} summary={summary}', file=sys.stderr)
    return status


if __name__ == '__main__': sys.exit(main())
