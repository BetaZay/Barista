import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('follow', Path(__file__).with_name('capture-radio-follow.py'))
follow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(follow)


def capture(payload=b'abc', endian='<', nano=False):
    magic = 0xa1b23c4d if nano else 0xa1b2c3d4
    return struct.pack(endian+'IHHIIII',magic,2,4,0,0,262144,127)+ \
        struct.pack(endian+'IIII',10,123000 if nano else 123,len(payload),len(payload))+payload


class PcapTests(unittest.TestCase):
    def test_fragmented_reads_and_multiple_sessions(self):
        out=io.BytesIO(); sink=follow.PcapSink(out)
        for endian,nano in (('<',False),('>',True)):
            for byte in capture(endian=endian,nano=nano): sink.feed(bytes([byte]))
            self.assertFalse(sink.end_segment())
        self.assertEqual(sink.packets,2)
        self.assertEqual(sink.first_us,10000123)
        self.assertEqual(out.getvalue(),capture()+capture()[24:])

    def test_partial_segment_does_not_poison_next(self):
        out=io.BytesIO(); sink=follow.PcapSink(out)
        sink.feed(capture()+capture()[24:-1])
        self.assertTrue(sink.end_segment())
        sink.feed(capture(b'xyz')); sink.end_segment()
        self.assertEqual(sink.partial_segments,1)
        self.assertEqual(sink.packets,2)
        self.assertEqual(out.getvalue(),capture()+capture(b'xyz')[24:])

    def test_reject_wrong_link_and_oversized_record(self):
        for bad in (capture()[:20]+struct.pack('<I',1)+capture()[24:],
                    capture()[:24]+struct.pack('<IIII',10,0,262145,262145)):
            with self.assertRaises(ValueError): follow.PcapSink(io.BytesIO()).feed(bad)


class LifecycleTests(unittest.TestCase):
    # A real pipe-writing subprocess simulates tcpdump. No network/sysfs writes.
    def setUp(self):
        self.out=io.BytesIO(); self.sink=follow.PcapSink(self.out)
        self.follower=follow.RadioFollower('drcdtsf',self.sink,timeout=0.01)
        self.popen=subprocess.Popen
        self.children=[]

    def tearDown(self):
        self.follower.close()
        for process in self.children:
            self.assertIsNotNone(process.poll(), 'leaked capture process')

    def spawn(self, command, **kwargs):
        body=repr(capture(bytes([len(self.children)+1])))
        program='import sys,signal,time; signal.signal(signal.SIGINT,lambda *_:sys.exit(0)); sys.stdout.buffer.write('+body+'); sys.stdout.buffer.flush(); time.sleep(60)'
        process=self.popen([sys.executable,'-u','-c',program],**kwargs)
        self.children.append(process)
        return process

    def collect(self, packets):
        until=time.monotonic()+3
        while self.sink.packets<packets and time.monotonic()<until: self.follower.step()
        self.assertEqual(self.sink.packets,packets)

    def test_same_name_new_ifindex_restarts_even_when_child_alive(self):
        with patch.object(follow.subprocess,'Popen',side_effect=self.spawn), \
             patch.object(self.follower,'interface_id',return_value=100) as identity:
            self.collect(1)
            first=self.children[0]
            self.assertIsNone(first.poll())
            identity.return_value=101
            self.collect(2)
            self.assertIsNotNone(first.poll())
            self.assertEqual(self.follower.identity,101)
        self.assertEqual(self.out.getvalue(),capture(b'\1')+capture(b'\2')[24:])

    def test_disappearance_then_reappearance(self):
        with patch.object(follow.subprocess,'Popen',side_effect=self.spawn), \
             patch.object(self.follower,'interface_id',return_value=100) as identity:
            self.collect(1)
            identity.return_value=None
            self.follower.step()
            self.assertIsNone(self.follower.process)
            identity.return_value=102
            self.collect(2)

    def test_missing_interface_timeout(self):
        with patch.object(self.follower,'interface_id',return_value=None):
            self.follower.step()
            self.follower.missing_since=time.monotonic()-1
            with self.assertRaisesRegex(RuntimeError,'monitor absent'): self.follower.step()

    def test_repeated_capture_failure_is_fatal(self):
        def fail(command, **kwargs):
            process=self.popen([sys.executable,'-c','raise SystemExit(2)'],**kwargs)
            self.children.append(process)
            return process
        with patch.object(follow.subprocess,'Popen',side_effect=fail), \
             patch.object(self.follower,'interface_id',return_value=100):
            with self.assertRaisesRegex(RuntimeError,'three times'):
                for _ in range(30): self.follower.step()


if __name__ == '__main__': unittest.main()
