"""Exercise non-systemd activation and upgrades without root or a booted VM."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "posix", "Requires Unix shell utilities and FIFOs")
class InitScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.service = self.root / "sv/barista"
        (self.service / "supervise").mkdir(parents=True)
        os.mkfifo(self.service / "supervise/control")
        self.marker = self.root / "upgrade"
        self.log = self.root / "commands"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        INIT_LOG=str(self.log))
        self.sv = self.executable("sv", '''#!/bin/sh
echo "$*" >> "$INIT_LOG"
case "$1" in
status) echo "${INIT_STATE:-run: barista: (pid 123) 10s}"; exit "${INIT_STATUS_RESULT:-0}" ;;
esac
case "$3" in
stop) exit "${INIT_STOP_RESULT:-0}" ;;
start) exit "${INIT_START_RESULT:-0}" ;;
esac
''')
        self.hooks = self.render("packaging/runit/package-hooks.in", "hooks")

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def render(self, source, name):
        text = (REPOSITORY / source).read_text()
        replacements = {
            "@BARISTA_RUNIT_SV@": str(self.sv),
            "@BARISTA_RUNIT_SERVICE_DIR@": str(self.service),
            "@CMAKE_INSTALL_FULL_LIBEXECDIR@": str(self.root / "libexec"),
            "/run/barista-package-upgrade": str(self.marker),
            "/run/barista": str(self.root / "runtime"),
            "/var/log/barista": str(self.root / "logs"),
            "/var/lib/barista": str(self.root / "state"),
        }
        for before, after in replacements.items():
            text = text.replace(before, after)
        path = self.root / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def invoke(self, command, **env):
        return subprocess.run(["sh", "-ec", '. "$1"; ' + command, "sh", str(self.hooks)],
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_active_upgrade_stops_and_restores_idle_service(self):
        result = self.invoke("barista_pre; barista_post")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), [
            f"status {self.service}", f"-w 30 stop {self.service}", f"-w 10 start {self.service}"])
        self.assertFalse(self.marker.exists())

    def test_administratively_down_service_stays_down(self):
        self.assertEqual(self.invoke("barista_pre; barista_post", INIT_STATE="down: barista: 10s").returncode, 0)
        self.assertNotIn("start", self.log.read_text())

    def test_unsupervised_service_is_ignored(self):
        (self.service / "supervise/control").unlink()
        self.assertEqual(self.invoke("barista_pre; barista_post").returncode, 0)
        self.assertFalse(self.log.exists())

    def test_pending_admin_stop_is_preserved(self):
        result = self.invoke("barista_pre; barista_post", INIT_STATE="run: barista: (pid 123) 10s, want down")
        self.assertEqual(result.returncode, 0)
        self.assertNotIn("start", self.log.read_text())

    def test_failed_status_aborts_without_marker(self):
        self.assertNotEqual(self.invoke("barista_pre", INIT_STATUS_RESULT="1").returncode, 0)
        self.assertFalse(self.marker.exists())

    def test_failed_stop_aborts_and_restores_service(self):
        self.assertNotEqual(self.invoke("barista_pre", INIT_STOP_RESULT="1").returncode, 0)
        self.assertIn("start", self.log.read_text())
        self.assertFalse(self.marker.exists())

    def test_interrupted_upgrade_preserves_restart_intent(self):
        self.marker.mkdir()
        (self.marker / "restart").touch()
        self.assertEqual(self.invoke("barista_pre; barista_post", INIT_STATE="down: barista: 10s").returncode, 0)
        self.assertIn("start", self.log.read_text())
        self.assertNotIn("status", self.log.read_text())

    def test_activation_uses_supervisor_and_rejects_upgrade(self):
        activate = self.render("packaging/runit/activate.in", "activate")
        result = subprocess.run([str(activate)], env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.log.read_text().strip(), f"-w 10 start {self.service}")
        self.log.unlink()
        self.marker.mkdir()
        result = subprocess.run([str(activate)], env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.log.exists())

    def test_activation_reports_supervisor_failure(self):
        activate = self.render("packaging/runit/activate.in", "activate")
        result = subprocess.run([str(activate)], env=dict(self.env, INIT_START_RESULT="1"))
        self.assertNotEqual(result.returncode, 0)

    def test_check_requires_bus_registration(self):
        self.executable("dbus-send", '#!/bin/sh\necho "boolean ${INIT_BUS_OWNER:-false}"\n')
        for owner, expected in [("false", 1), ("true", 0)]:
            result = subprocess.run(["sh", str(REPOSITORY / "packaging/runit/check")],
                                    env=dict(self.env, INIT_BUS_OWNER=owner))
            self.assertEqual(result.returncode, expected)
        self.executable("dbus-send", "#!/bin/sh\nexit 1\n")
        self.assertNotEqual(subprocess.run(["sh", str(REPOSITORY / "packaging/runit/check")],
                                          env=self.env).returncode, 0)

    def test_launcher_creates_directories_and_executes_daemon(self):
        daemon = self.root / "libexec/barista/barista-service"
        daemon.parent.mkdir(parents=True)
        daemon.write_text('#!/bin/sh\necho daemon >> "$INIT_LOG"\nexit 23\n')
        daemon.chmod(0o755)
        launcher = self.render("packaging/linux/service-launch.in", "launcher")
        result = subprocess.run([str(launcher)], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(self.log.read_text().strip(), "daemon")
        for name, mode in [("runtime", 0o755), ("logs", 0o755), ("state", 0o700)]:
            self.assertEqual((self.root / name).stat().st_mode & 0o777, mode)

    def test_failed_directory_setup_never_launches_daemon(self):
        self.executable("install", "#!/bin/sh\nexit 1\n")
        launcher = self.render("packaging/linux/service-launch.in", "launcher")
        self.assertNotEqual(subprocess.run([str(launcher)], env=self.env,
                                          capture_output=True).returncode, 0)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
