"""Capture lifecycle fixtures: no root privileges or real radios required."""
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('capture', Path(__file__).with_name('barista-capture-helper.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class WorkflowTests(unittest.TestCase):
    def test_client_disconnect_signals_cleanup(self):
        with patch.object(capture.os, 'read', side_effect=[b'x', b'']), \
                patch.object(capture.os, 'kill') as kill:
            capture.watch_client()
            kill.assert_called_once_with(capture.os.getpid(), capture.signal.SIGTERM)

    def test_reports_include_summary_without_key_material(self):
        delivered = dict(matched_video_frames_complete=10, matched_video_frames_incomplete=1,
                         block_ack_evidence={'video_frames_fully_block_acked': 9},
                         counts={'unique_air_recovery_requests': 2}, limitations='Observation only')
        clock = dict(format_age_us={'median': 1700}, format_age_at_least_8000us=0,
                     format_to_same_timestamp_video_first_us={'median': 5000})
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(capture.decrypt, 'decrypt_with_pmk', return_value=0), \
                patch.object(capture.delivery, 'analyze', return_value=delivered), \
                patch.object(capture.timing, 'analyze', return_value=clock), \
                patch.object(capture.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='{}')), \
                patch('sys.stdout', new_callable=io.StringIO) as output:
            folder = Path(tmp)
            capture.analyze(folder, '40:d2:8a:bf:fc:a8', '40:d2:8a:ab:90:00', b'private-fixture-key')
            summary = __import__('json').loads((folder / 'summary.json').read_text())
            self.assertEqual(summary['recovery_requests'], 2)
            self.assertEqual(summary['fully_block_acked_frames'], 9)
            self.assertNotIn('private-fixture-key', output.getvalue())
            self.assertTrue((folder / 'format-clock.json').exists())

    def test_invalid_interface_names_rejected_before_radio_calls(self):
        for name in ('../wlan0', 'wlan0;id', '-a', 'x' * 16):
            with self.assertRaises(ValueError):
                capture.validate_interfaces(name, 'wlan1')

    def test_capture_restores_radio_and_publishes_outputs_after_analysis(self):
        self.lifecycle(False)

    def test_capture_failure_restores_radio_without_analysis(self):
        self.lifecycle(True)

    def lifecycle(self, fail):
        calls, processes, ownership = [], [], []
        class Process:
            def __init__(self, args, **kwargs):
                self.returncode = None
                self.args = args
                processes.append(self)
            def poll(self):
                return self.returncode
            def send_signal(self, value):
                self.returncode = 0
            def wait(self, timeout=None):
                if '-c' in self.args and fail and self.returncode is None:
                    raise subprocess.TimeoutExpired(self.args, timeout)
                self.returncode = 0
                return 0
        def command(*args, **kwargs):
            calls.append(args)
            if args[:3] == ('iw', 'dev', 'wlan0'):
                return SimpleNamespace(stdout='addr 40:d2:8a:bf:fc:a8\n type AP\n channel 149 (5745 MHz)')
            return SimpleNamespace(stdout='yes')
        real_read = Path.read_text
        real_exists = Path.exists
        def exists(path):
            if path == Path('/sys/class/net', capture.MONITOR):
                return False
            return real_exists(path)
        def read(path, *args, **kwargs):
            if path.name == 'credentials.conf':
                return 'psk=' + '0' * 64 + '\ngamepad_mac=40:d2:8a:ab:90:00\n'
            if path.name == 'flags':
                return '0x1'
            return real_read(path, *args, **kwargs)
        def analyzed(folder, *args):
            self.assertEqual(ownership, [])
            capture.save_report(folder / 'analysis.json', {'ok': True})
        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(capture, 'BASE', Path(tmp)), \
                patch.object(capture, 'command', side_effect=command), \
                patch.object(Path, 'read_text', read), \
                patch.object(Path, 'exists', exists), \
                patch.object(capture.subprocess, 'Popen', Process), \
                patch.object(capture.time, 'sleep'), \
                patch.object(capture.time, 'monotonic', side_effect=[0, 0, 1000]), \
                patch.object(capture.os, 'chown', side_effect=lambda *args: ownership.append(args)), \
                patch.object(capture, 'analyze', side_effect=analyzed) as analysis, \
                patch('sys.stdout', new_callable=io.StringIO):
            args = SimpleNamespace(ap='wlan0', radio='wlan1', seconds=10)
            if fail:
                with self.assertRaises(subprocess.TimeoutExpired):
                    capture.capture(args, 1000)
                analysis.assert_not_called()
            else:
                self.assertEqual(capture.capture(args, 1000), 0)
                analysis.assert_called_once()
            self.assertIn(('iw', 'dev', capture.MONITOR, 'del'), calls)
            self.assertIn(('ip', 'link', 'set', 'wlan1', 'up'), calls)
            self.assertEqual(calls[-1], ('nmcli', 'device', 'set', 'wlan1', 'managed', 'yes'))
            self.assertTrue(all(p.poll() is not None for p in processes))
            self.assertTrue(ownership)
            folder = next(Path(tmp).iterdir())
            self.assertEqual(folder.stat().st_mode & 0o777, 0o700)
            metadata = __import__('json').loads((folder / 'session.json').read_text())
            self.assertEqual(metadata['capture_complete'], not fail)
            self.assertNotIn('psk', metadata)


if __name__ == '__main__':
    unittest.main()
