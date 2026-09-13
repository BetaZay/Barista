import datetime as dt
import json
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

import repositories
from version import assign


class RepositoryTests(unittest.TestCase):
    @patch("repositories.time.sleep")
    @patch("repositories.subprocess.run")
    def test_transient_github_errors_are_retried(self, process, sleep):
        process.side_effect = [
            subprocess.CompletedProcess(("gh", "api"), 1, "", "HTTP 500 (example)\n"),
            subprocess.CompletedProcess(("gh", "api"), 0, "result\n", ""),
        ]
        self.assertEqual(repositories.run("gh", "api"), "result")
        self.assertEqual(process.call_count, 2)
        sleep.assert_called_once_with(2)

    @patch("repositories.time.sleep")
    @patch("repositories.subprocess.run")
    def test_non_transient_github_errors_are_not_retried(self, process, sleep):
        process.return_value = subprocess.CompletedProcess(("gh", "api"), 1, "", "HTTP 422 (example)\n")
        with self.assertRaises(subprocess.CalledProcessError):
            repositories.run("gh", "api")
        self.assertEqual(process.call_count, 1)
        sleep.assert_not_called()

    def test_assignment(self):
        self.assertEqual(assign("0.1", "147"), "0.1.147")
        self.assertEqual(assign("0.2", "148"), "0.2.148")
        for series, counter in [("0.1.0", "1"), ("01.1", "1"), ("0.1", "0"), ("0.1", "1;exit")]:
            with self.assertRaises(ValueError):
                assign(series, counter)

    def test_ordering(self):
        self.assertLess(repositories.version("0.1.99"), repositories.version("0.1.100"))
        for value in ("0.1.0", "v0.1.123", "0.1.1-beta", "0.1.01", "../evil"):
            with self.assertRaises(ValueError):
                repositories.version(value)

    def test_retention(self):
        now = dt.datetime.now(dt.timezone.utc)
        old = (now - dt.timedelta(days=30)).isoformat()
        builds = {f"0.1.{i}": {"published": old} for i in range(1, 31)}
        builds["0.1.7"]["published"] = now.isoformat()
        keep = repositories.retained(builds, ["0.1.1", "0.1.2", "0.1.3", "0.1.4"], now)
        self.assertEqual(keep, {f"0.1.{i}" for i in range(21, 31)} | {"0.1.2", "0.1.3", "0.1.4", "0.1.7"})

    @patch("repositories.run")
    def test_archive_checksums(self, run):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            package = directory / "package.deb"
            package.write_bytes(b"tested bytes")
            record = {"file": package.name, "sha256": repositories.digest(package)}
            manifest = {"version": "0.1.1", "packages": {t: record for t in repositories.TARGETS}}
            (directory / "build.json").write_text(json.dumps(manifest))
            self.assertEqual(repositories.verify_archive(directory), manifest)
            package.write_bytes(b"tampered")
            with self.assertRaises(ValueError):
                repositories.verify_archive(directory)

    @patch("repositories.run", side_effect=RuntimeError("bad signature"))
    def test_invalid_signature_rejected(self, run):
        with self.assertRaises(RuntimeError):
            repositories.verify_archive(Path("unused"))


if __name__ == "__main__":
    unittest.main()
