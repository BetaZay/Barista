"""Exercise every launcher profile without sudo, Wi-Fi changes or output files."""
import os
from pathlib import Path
import shlex
import subprocess
import unittest

SCRIPT = Path(__file__).with_name("test-media-profile.sh")


class Profiles(unittest.TestCase):
    def run_profile(self, *args):
        return subprocess.run(
            ["bash", str(SCRIPT), *args], capture_output=True, text=True,
            env={**os.environ, "DRCD_TEST_DRY_RUN": "1", "DRCD_AP_TSF_CLOCK": "0",
                 "DRCD_ALL_IDR": "1", "DRCD_REFERENCE_AUDIO_TIME": "1",
                 "DRCD_LEGACY_ENCODER_QUALITY": "1",
                 "DRCD_INTRA_REFRESH": "0", "DRCD_CEMU_SOCKET": "/old.sock"},
            check=False,
        )

    def test_matrix(self):
        for profile in ("baseline", "all-idr", "audio-time", "combined", "burst"):
            for source in ("cemu", "generated"):
                with self.subTest(profile=profile, source=source):
                    result = self.run_profile(profile, source, "/tmp/profile test.pcap")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    command = shlex.split(result.stdout.splitlines()[-1])
                    self.assertEqual(command[:3], ["env", "-u", "DRCD_CEMU_SOCKET"])
                    self.assertIn("DRCD_REAL_REPLAY", command[:5])
                    settings = dict(item.split("=", 1) for item in command[3:] if item.startswith("DRCD_") and "=" in item)
                    self.assertEqual(settings["DRCD_AP_TSF_CLOCK"], "1")
                    self.assertEqual(settings["DRCD_VIDEO_QP"], "32")
                    self.assertEqual(settings["DRCD_INTRA_REFRESH"], "1")
                    self.assertEqual(settings["DRCD_LEGACY_ENCODER_QUALITY"], "0")
                    self.assertEqual(settings["DRCD_SEND_TIME_VIDEO"], "1")
                    self.assertEqual(settings["DRCD_IDR_INIT"], "1")
                    self.assertEqual(settings["DRCD_ALL_IDR"], str(int(profile in ("all-idr", "combined"))))
                    self.assertEqual(settings["DRCD_REFERENCE_AUDIO_TIME"], str(int(profile in ("audio-time", "combined"))))
                    self.assertEqual(settings["DRCD_CHUNK_PACING"], str(int(profile != "burst")))
                    self.assertEqual(settings["DRCD_GENERATED_AV"], str(int(source == "generated")))
                    self.assertEqual("DRCD_CEMU_SOCKET" in settings, source == "cemu")
                    self.assertEqual("--black" in command, source == "generated")
                    self.assertIn("/tmp/profile test.pcap", command)
                    self.assertEqual(settings["DRCD_LOG_FILE"], "/tmp/profile test.log")

    def test_bad_arguments(self):
        for args in (("typo",), ("baseline", "typo"), ("baseline", "cemu", "a", "extra")):
            self.assertEqual(self.run_profile(*args).returncode, 2)


if __name__ == "__main__":
    unittest.main()
