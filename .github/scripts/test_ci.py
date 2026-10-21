import unittest
import re
import shlex
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

from changes import docs_only, requires_build
from check_docs import missing_links


class ChangeDetectionTests(unittest.TestCase):
    def test_docs(self):
        self.assertTrue(docs_only(["README.md", "docs/user-guide.md", "docs/screenshots/home.png"]))

    def test_code_and_unknown_paths_require_build(self):
        for paths in [[], ["app/gui/window.cpp"], ["README.md", ".github/workflows/continuous.yml"],
                      ["docs/example.cpp"], ["packaging/arch/PKGBUILD"], ["CMakeLists.txt"]]:
            self.assertFalse(docs_only(paths))

    def test_manual_and_unknown_history_require_build(self):
        self.assertTrue(requires_build("workflow_dispatch", {}))
        self.assertTrue(requires_build("merge_group", {}))
        self.assertTrue(requires_build("push", {}))
        self.assertTrue(requires_build("push", {"before": "0" * 40}))

    @patch("changes.subprocess.check_output", return_value=b"README.md\0docs/hardware.md\0")
    def test_push_docs(self, command):
        self.assertFalse(requires_build("push", {"before": "abc"}))

    @patch("changes.subprocess.check_output", side_effect=["base\n", b"README.md\0app/main.cpp\0"])
    def test_pr_uses_merge_base(self, command):
        self.assertTrue(requires_build("pull_request", {"pull_request": {"base": {"sha": "abc"}}}))
        self.assertEqual(command.call_args_list[0].args[0], ["git", "merge-base", "abc", "HEAD"])

    @patch("changes.subprocess.check_output", side_effect=subprocess.CalledProcessError(1, "git"))
    def test_missing_git_history_fails_closed(self, command):
        self.assertTrue(requires_build("push", {"before": "missing"}))


class DocumentationTests(unittest.TestCase):
    def test_local_links_images_and_external_urls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "README.md"
            (root / "guide.md").write_text("# Guide")
            file.write_text('[Guide](guide.md#section) [Web](https://example.com) '
                            '[Missing](missing.md) ![Image](missing.png) '
                            '<img src="logo.png">')
            errors = missing_links(root, file)
            self.assertEqual(len(errors), 3)
            self.assertTrue(any("missing.md" in error for error in errors))

    def test_fenced_examples_are_not_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "README.md"
            file.write_text('```md\n[example](not-a-real-file.md)\n```')
            self.assertEqual(missing_links(root, file), [])


class BuildDependencyTests(unittest.TestCase):
    @staticmethod
    def installed_packages(job, manager):
        root = Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/continuous.yml").read_text()
        section = re.search(r"(?ms)^  " + re.escape(job) + r":\n(.*?)(?=^  [\w-]+:\n|\Z)", workflow)[1]
        for line in section.replace("\\\n", " ").splitlines():
            if manager not in line:
                continue
            words = shlex.split(line)
            if manager not in words:
                continue
            index = words.index(manager)
            if manager == "pacman" and "-Syu" in words:
                return set(word for word in words[index + 1:] if not word.startswith("-"))
            if "install" in words[index + 1:]:
                return set(word for word in words[words.index("install", index) + 1:] if not word.startswith("-"))
        raise AssertionError(f"No dependency installer found for {job}")

    def test_capture_test_dependencies_are_explicit(self):
        for job, manager, dependency in (("test", "apt-get", "python3-cryptography"),
                                          ("deb", "apt-get", "python3-cryptography"),
                                          ("rpm", "dnf", "python3-cryptography")):
            with self.subTest(job=job):
                self.assertIn(dependency, self.installed_packages(job, manager))

    def test_arch_installs_package_dependencies_before_makepkg(self):
        root = Path(__file__).resolve().parents[2]
        pkgbuild = (root / "packaging/arch/PKGBUILD").read_text()
        required = set()
        for field in ("depends", "checkdepends"):
            required.update(shlex.split(re.search(r"(?m)^" + field + r"=\((.*?)\)$", pkgbuild)[1]))
        installed = self.installed_packages("arch", "pacman")
        self.assertFalse(required - installed, f"Arch CI dependencies missing: {sorted(required - installed)}")


if __name__ == "__main__":
    unittest.main()
