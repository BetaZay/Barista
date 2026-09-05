"""Rate-test orchestration checks using fake iw/capture processes, never hardware."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import contextlib
import io

spec = importlib.util.spec_from_file_location("rate_test", Path(__file__).with_name("test-wifi-rate.py"))
rate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rate)
INFO = "Interface wlan0\n\ttype AP\n\tchannel 36 (5180 MHz), width: 20 MHz\n"
STATION = "Station 40:d2:8a:ab:90:00\n\tauthorized:\tyes\n\ttx bitrate: 58.5 MBit/s MCS 6\n"


class RateTests(unittest.TestCase):
    def test_stream_readiness_requires_all_markers(self):
        markers = ["DRC session ready: synchronized", "Media first frame: 5 chunks, sends OK",
                   "GamePad streaming state: active"]
        def ready(lines):
            return rate.streaming_ready(SimpleNamespace(read_text=lambda **kwargs: "\n".join(lines)))
        self.assertTrue(ready(markers))
        for i in range(3):
            self.assertFalse(ready(markers[:i] + markers[i + 1:]))
        self.assertFalse(ready([markers[0], markers[1].replace("OK", "FAILED"), markers[2]]))
        self.assertFalse(ready(markers + ["GamePad streaming state: waiting"]))

    def test_old_session_readiness_is_discarded(self):
        good = "DRC session ready: yes\nMedia first frame: 5 chunks, sends OK\nGamePad streaming state: active\n"
        for reset in ("stop-session:", "AP-STA-DISCONNECTED", "AP-STA-CONNECTED",
                      "runtime-start: AP-ENABLED", "media: stopped"):
            log = SimpleNamespace(read_text=lambda **kwargs: good + reset + "\nGamePad streaming state: active")
            self.assertFalse(rate.streaming_ready(log))

    def test_validation(self):
        for mcs in (4, 5, 6):
            self.assertEqual(rate.rate_command("wlan0", mcs),
                ["iw", "dev", "wlan0", "set", "bitrates", "ht-mcs-5", str(mcs), "lgi-5"])
        for args in (["7", "/tmp/a.pcap"], ["6", "/tmp/a.pcap", "--interface", "x;bad"],
                     ["6", "/tmp/a.pcap", "--seconds", "0"], ["6", "/tmp/a.pcap", "--pad", "bad"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                rate.parse_args(args)

    def test_link_gate(self):
        self.assertTrue(rate.valid_link(INFO, STATION))
        self.assertFalse(rate.valid_link(INFO.replace("5180", "2412"), STATION))
        self.assertFalse(rate.valid_link(INFO.replace("type AP", "type managed"), STATION))
        self.assertFalse(rate.valid_link(INFO, STATION.replace("yes", "no")))

    def test_dry_run_has_no_effects(self):
        args = rate.parse_args(["6", "/tmp/not-created-rate-test.pcap", "--dry-run"])
        with patch.object(rate.subprocess, "run") as run, patch.object(rate.subprocess, "Popen") as popen:
            with contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(rate.run(args), 0)
            self.assertEqual(json.loads(output.getvalue())["test_seconds"], 30)
            run.assert_not_called()
            popen.assert_not_called()

    def simulate(self, mode="success"):
        clock = SimpleNamespace(now=0.0)
        changed = []
        child = SimpleNamespace(returncode=None)
        child.poll = lambda: child.returncode
        def stop(sig):
            child.returncode = 130
        child.send_signal = stop
        child.wait = lambda timeout: child.returncode
        def execute(command, **kwargs):
            if command[0] == "pgrep":
                return SimpleNamespace(returncode=0 if mode == "already_running" else 1)
            if "bitrates" in command:
                changed.append(clock.now)
                return SimpleNamespace(returncode=1 if mode == "rejected" else 0, stdout="", stderr="")
            if command[-1] == "info":
                text = INFO
            else:
                text = "" if mode == "no_station" or (mode == "disconnect" and changed) else STATION
            return SimpleNamespace(returncode=0, stdout=text, stderr="")
        def sleep(seconds):
            clock.now += seconds
        def ready(path):
            if mode == "authorized_no_video": return False
            if mode == "startup_retry" and 1 <= clock.now < 3: return False
            return True
        with tempfile.TemporaryDirectory() as directory:
            args = rate.parse_args(["6", directory + "/run.pcap", "--baseline-seconds", "5",
                                    "--seconds", "5", "--connect-timeout", "10"])
            with patch.object(rate.os, "geteuid", return_value=0), \
                 patch.object(rate.shutil, "which", return_value="tool"), \
                 patch.object(rate.subprocess, "run", side_effect=execute), \
                 patch.object(rate.subprocess, "Popen", return_value=child) as launch, \
                 patch.object(rate, "streaming_ready", side_effect=ready), \
                 patch.object(rate.time, "monotonic", side_effect=lambda: clock.now), \
                 patch.object(rate.time, "sleep", side_effect=sleep), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                if mode == "already_running":
                    with self.assertRaises(RuntimeError):
                        rate.run(args)
                    launch.assert_not_called()
                    return
                result = rate.run(args)
            events = [json.loads(line) for line in args.output.with_suffix(".rate.jsonl").read_text().splitlines()]
            self.assertEqual(child.returncode, 130)
            self.assertEqual(events[-1]["event"], "finished")
            self.assertTrue(all("unix_ns" in e and "monotonic_ns" in e for e in events))
            if mode in ("success", "startup_retry"):
                self.assertEqual(result, 0)
                self.assertEqual(changed, [8.0 if mode == "startup_retry" else 5.0])
                self.assertEqual(clock.now, 13.0 if mode == "startup_retry" else 10.0)
                names = [e["event"] for e in events]
                self.assertLess(names.index("streaming_ready"), names.index("rate_request"))
                self.assertLess(names.index("baseline_end"), names.index("rate_request"))
                self.assertEqual(names.count("baseline_restart"), int(mode == "startup_retry"))
            else:
                self.assertEqual(result, 1)
                if mode in ("no_station", "authorized_no_video"): self.assertEqual(changed, [])

    def test_full_run(self): self.simulate()
    def test_rejected_rate_stops_capture(self): self.simulate("rejected")
    def test_disconnect_stops_capture(self): self.simulate("disconnect")
    def test_missing_station_never_changes_rate(self): self.simulate("no_station")
    def test_existing_daemon_rejected(self): self.simulate("already_running")
    def test_authorized_without_video_never_changes_rate(self): self.simulate("authorized_no_video")
    def test_startup_retry_restarts_baseline_without_aborting(self): self.simulate("startup_retry")


if __name__ == "__main__":
    unittest.main()
