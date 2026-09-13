"""Exercise package lifecycle without systemd, root privileges, or real services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class PackageHookTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "systemd").mkdir()
        self.log = self.root / "commands"
        script = (Path(__file__).resolve().parents[2] / "packaging/linux/package-hooks.sh").read_text()
        script = script.replace("/run/systemd/system", str(self.root / "systemd"))
        script = script.replace("/run/barista-package-upgrade", str(self.root / "marker"))
        self.hooks = self.root / "hooks"
        self.hooks.write_text(script)
        systemctl = self.root / "systemctl"
        systemctl.write_text('''#!/bin/sh
echo "$*" >> "$HOOK_LOG"
case "$1" in
is-enabled) echo "${HOOK_STATE:-enabled}" ;;
stop) exit "${HOOK_STOP_RESULT:-0}" ;;
show) echo "${HOOK_RESULT:-success}" ;;
mask) exit "${HOOK_MASK_RESULT:-0}" ;;
esac
''')
        systemctl.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ["PATH"], HOOK_LOG=str(self.log))

    def invoke(self, command, **env):
        return subprocess.run(["sh", "-ec", '. "$1"; ' + command, "sh", str(self.hooks)],
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_stop_mask_reload_order(self):
        self.assertEqual(self.invoke("barista_pre; barista_post").returncode, 0)
        self.assertEqual(self.log.read_text().splitlines(), [
            "is-enabled barista.service", "mask --runtime barista.service", "stop barista.service",
            "show --property=Result --value barista.service", "unmask --runtime barista.service", "daemon-reload"])
        self.assertFalse((self.root / "marker").exists())

    def test_preserve_admin_mask_and_still_stop(self):
        self.assertEqual(self.invoke("barista_pre; barista_post", HOOK_STATE="masked").returncode, 0)
        commands = self.log.read_text()
        self.assertIn("stop barista.service", commands)
        self.assertNotIn("unmask", commands)
        self.assertNotIn("mask --runtime", commands)

    def test_failed_stop_aborts_and_restores_activation(self):
        self.assertNotEqual(self.invoke("barista_pre", HOOK_STOP_RESULT="1").returncode, 0)
        self.assertIn("unmask --runtime", self.log.read_text())
        self.assertFalse((self.root / "marker").exists())

    def test_failed_mask_aborts(self):
        self.assertNotEqual(self.invoke("barista_pre", HOOK_MASK_RESULT="1").returncode, 0)
        self.assertNotIn("stop barista.service", self.log.read_text())
        self.assertFalse((self.root / "marker").exists())

    def test_timeout_is_not_graceful_shutdown(self):
        self.assertNotEqual(self.invoke("barista_pre", HOOK_RESULT="timeout").returncode, 0)

    def test_interrupted_transaction_recovers_own_mask(self):
        (self.root / "marker").mkdir()
        self.assertEqual(self.invoke("barista_pre; barista_post", HOOK_STATE="masked-runtime").returncode, 0)
        self.assertIn("unmask --runtime", self.log.read_text())

    def test_container_no_systemd(self):
        (self.root / "systemd").rmdir()
        self.assertEqual(self.invoke("barista_pre; barista_post").returncode, 0)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
