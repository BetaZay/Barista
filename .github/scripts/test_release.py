"""Exercise publication ordering with a fake gh; never contact GitHub."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("publish-release.sh").resolve()
FAKE_GH = '''#!/usr/bin/env python3
import os, sys
from pathlib import Path
args = " ".join(sys.argv[1:])
log = Path("calls.log")
previous = log.read_text() if log.exists() else ""
with log.open("a") as output:
    output.write(args + "\\n")
if "git/ref/heads/main" in args:
    stale = os.environ.get("STALE") == "1" or (os.environ.get("ADVANCE") == "1" and "release upload" in previous)
    print("newer" if stale else os.environ["SHA"])
elif args.startswith("release upload"):
    sys.exit(int(os.environ.get("UPLOAD_FAIL", "0")))
elif args.startswith("api --paginate") and "/assets" in args:
    print("71\\n72")
elif args.startswith("api --paginate"):
    if os.environ.get("FIRST") != "1": print("12")
elif "git/matching-refs" in args:
    if os.environ.get("FIRST") != "1": print("refs/tags/continuous")
'''


class ReleaseTests(unittest.TestCase):
    def run_release(self, **flags):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            gh = root / "bin/gh"
            gh.write_text(FAKE_GH)
            gh.chmod(0o755)
            (root / "dist").mkdir()
            for suffix in ("deb", "rpm", "pkg.tar.zst"):
                (root / f"dist/barista-x86_64.{suffix}").write_text("fixture")
            env = dict(os.environ, GH_REPO="test/repo", SHA="abcdef1234567890",
                       GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1", TMPDIR=directory,
                       PATH=str(root / "bin") + os.pathsep + os.environ["PATH"], **flags)
            result = subprocess.run(["bash", str(SCRIPT)], cwd=root, env=env, capture_output=True, text=True)
            return result, (root / "calls.log").read_text()

    def test_stale_run_does_not_mutate_release(self):
        result, calls = self.run_release(STALE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("release upload", calls)
        self.assertNotIn("--method", calls)

    def test_failed_upload_preserves_existing_assets_and_tag(self):
        result, calls = self.run_release(UPLOAD_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("--method", calls)
        self.assertNotIn("release edit", calls)
        self.assertNotIn("--clobber", calls)

    def test_success_uploads_before_updating_and_pruning(self):
        result, calls = self.run_release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(calls.index("release upload"), calls.index("--method PATCH"))
        self.assertLess(calls.index("release edit"), calls.index("--method DELETE"))
        self.assertNotIn("release delete", calls)

    def test_first_release_is_a_draft_until_upload_finishes(self):
        result, calls = self.run_release(FIRST="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--draft", calls)
        self.assertIn("--method POST", calls)
        self.assertLess(calls.index("release upload"), calls.index("release edit"))
        self.assertNotIn("--method DELETE", calls)

    def test_main_advancing_during_upload_preserves_previous_release(self):
        result, calls = self.run_release(ADVANCE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("release upload", calls)
        self.assertNotIn("release edit", calls)
        self.assertNotIn("--method", calls)
