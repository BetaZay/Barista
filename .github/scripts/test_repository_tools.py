"""Opt-in integration test using real signing and repository tools, fake GitHub.

Run only in a disposable container: BARISTA_REPOSITORY_TOOLS_TEST=1 python3
-m unittest discover -s .github/scripts -p test_repository_tools.py
"""
import fnmatch
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import repositories


@unittest.skipUnless(os.environ.get("BARISTA_REPOSITORY_TOOLS_TEST") == "1", "Requires disposable repository-tools container")
class RepositoryToolsTests(unittest.TestCase):
    def test_publish_promote_and_authenticate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "keys"
            home.mkdir(mode=0o700)
            env = dict(os.environ, GNUPGHOME=str(home), GH_REPO="test/Barista",
                       GITHUB_RUN_ID="123", RELEASE_VERSION="0.1.123", RELEASE_SOURCE="a" * 40,
                       PROMOTE_VERSION="")
            with patch.dict(os.environ, env):
                subprocess.run(["gpg", "--batch", "--passphrase", "", "--quick-generate-key",
                                "Barista repository test <test@example.invalid>", "rsa2048", "sign", "1d"], check=True)
                listing = repositories.run("gpg", "--with-colons", "--list-secret-keys")
                os.environ["SIGNING_FINGERPRINT"] = next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:"))
                original = Path.cwd()
                workspace = root / "work"
                workspace.mkdir()
                os.chdir(workspace)
                try:
                    self.exercise(root)
                finally:
                    os.chdir(original)
                    subprocess.run(["gpgconf", "--kill", "gpg-agent"], check=False)

    def exercise(self, root):
        dist = Path("dist")
        dist.mkdir()
        deb = Path("deb/DEBIAN")
        deb.mkdir(parents=True)
        (deb / "control").write_text("Package: barista\nVersion: 0.1.123-1\nArchitecture: amd64\nMaintainer: Test <test@example.invalid>\nDescription: Repository format test\n")
        repositories.run("dpkg-deb", "--root-owner-group", "--build", "deb", "dist/barista.deb")
        rpmroot = root / "rpmbuild"
        for name in ("BUILD", "BUILDROOT", "RPMS", "SOURCES", "SPECS", "SRPMS"):
            (rpmroot / name).mkdir(parents=True)
        spec = rpmroot / "SPECS/test.spec"
        spec.write_text("""Name: barista
Version: 0.1.123
Release: 1
Summary: Repository format test
License: MIT
%description
Repository format test.
%install
mkdir -p %{buildroot}/usr/share/barista
echo test > %{buildroot}/usr/share/barista/test
%files
/usr/share/barista/test
""")
        repositories.run("rpmbuild", "--define", "_topdir " + str(rpmroot), "-bb", str(spec))
        shutil.copy2(next((rpmroot / "RPMS").rglob("*.rpm")), dist / "barista.rpm")
        arch = Path("arch")
        arch.mkdir()
        (arch / ".PKGINFO").write_text("pkgname = barista\npkgbase = barista\npkgver = 0.1.123-1\npkgdesc = Repository format test\nurl = https://example.invalid\nbuilddate = 1700000000\npackager = Test\nsize = 0\narch = x86_64\nlicense = MIT\n")
        repositories.run("tar", "--zstd", "-cf", "dist/barista.pkg.tar.zst", "-C", "arch", ".PKGINFO")
        real_run = repositories.run
        records = {}
        store = root / "remote"
        store.mkdir()

        def fake_run(*args, cwd=None):
            if args[0] == "docker":
                directory = Path(args[4].split(":")[0])
                return real_run("repo-add", "barista.db.tar.gz", *[p.name for p in directory.glob("*.pkg.tar.zst")], cwd=directory)
            if args[0] != "gh":
                return real_run(*args, cwd=cwd)
            if args[1] == "api":
                return json.dumps([list(records.values())])
            command, tag = args[2:4]
            remote = store / tag
            if command == "create":
                records[tag] = {"tag_name": tag, "draft": "--draft" in args}
                remote.mkdir()
            elif command == "upload":
                for file in args[4:]:
                    shutil.copy2(file, remote / Path(file).name)
            elif command == "edit":
                records[tag]["draft"] = False
            elif command == "download":
                directory = Path(args[args.index("--dir") + 1])
                pattern = args[args.index("--pattern") + 1] if "--pattern" in args else "*"
                for file in remote.iterdir():
                    if fnmatch.fnmatch(file.name, pattern):
                        shutil.copy2(file, directory / file.name)
            else:
                self.fail(f"Unexpected fake GitHub command: {args}")
            return ""

        with patch.object(repositories, "run", side_effect=fake_run):
            repositories.prepare()
            preview = json.loads(Path("site/updates/v1.json").read_text())
            self.assertNotIn("stable", preview["channels"])
            self.assertEqual(Path("site/install.sh").read_bytes(), Path("install.sh").read_bytes())
            self.assertTrue(Path("site/install.sh").stat().st_mode & 0o111)
            archive = repositories.verify_archive(Path("archives/0.1.123"))
            self.assertEqual(archive["version"], "0.1.123")
            # Validate actual signed APT, RPM metadata, and pacman databases.
            real_run("gpg", "--verify", "site/apt/ubuntu/noble/dists/preview/InRelease")
            real_run("gpg", "--verify", "site/rpm/fedora/44/preview/x86_64/repodata/repomd.xml.asc",
                     "site/rpm/fedora/44/preview/x86_64/repodata/repomd.xml")
            real_run("gpg", "--verify", "site/arch/preview/x86_64/barista.db.sig", "site/arch/preview/x86_64/barista.db")
            real_run("rpm", "--import", str(Path("site/barista.asc").resolve()))
            signed_rpm = next(Path("site/rpm").rglob("*.rpm"))
            real_run("rpmkeys", "--checksig", str(signed_rpm))
            aptlist = root / "barista.list"
            aptlist.write_text(f"deb [signed-by={Path('site/barista.asc').resolve()}] file://{Path('site/apt/ubuntu/noble').resolve()} preview main\n")
            lists = root / "lists"
            lists.mkdir()
            apt_args = ("apt-get", "-o", "APT::Sandbox::User=root", "-o", "Dir::Etc::sourceparts=-",
                        "-o", "Dir::Etc::sourcelist=" + str(aptlist), "-o", "Dir::State::lists=" + str(lists))
            real_run(*apt_args, "update")
            installed = root / "installed"
            (installed / "var/lib/dpkg").mkdir(parents=True)
            for candidate in ("0.1.0", "0.1.0.r42.gabcdef"):
                real_run("dpkg", "--compare-versions", candidate, "lt", "0.1.123-1")

            def install_deb(number):
                download = root / ("download-" + number)
                download.mkdir()
                real_run(*apt_args, "-o", "APT::Update::Error-Mode=any", "update")
                real_run(*apt_args, "download", "barista=" + number + "-1", cwd=download)
                real_run("dpkg", "--root=" + str(installed), "--install", str(next(download.glob("*.deb"))))
                actual = real_run("dpkg-query", "--admindir=" + str(installed / "var/lib/dpkg"), "-W", "-f=${Version}", "barista")
                self.assertEqual(actual, number + "-1")

            install_deb("0.1.123")
            shutil.rmtree("site")
            # Same run, same version, same unsigned inputs: archive is reused.
            repositories.prepare()
            shutil.rmtree("site")
            os.environ["RELEASE_VERSION"] = ""
            os.environ["PROMOTE_VERSION"] = "0.1.123"
            repositories.prepare()
            promoted = json.loads(Path("site/updates/v1.json").read_text())
            self.assertEqual(promoted["channels"]["stable"], promoted["channels"]["preview"])
            for item in archive["packages"].values():
                matches = list(Path("site").rglob(item["file"]))
                self.assertEqual(len(matches), 2)
                self.assertTrue(all(repositories.digest(p) == item["sha256"] for p in matches))
            self.assertIn("stable-intent-0.1.123", records)
            # Simulate failure after deployment but before finalize; refresh
            # completes intent rather than reverting the Stable channel.
            shutil.rmtree("site")
            os.environ["PROMOTE_VERSION"] = ""
            repositories.prepare()
            repositories.finalize()
            self.assertIn("v0.1.123", records)
            # Native signature verification must reject damaged RPM bytes.
            damaged = Path("damaged.rpm")
            damaged.write_bytes(signed_rpm.read_bytes()[:-32] + b"x" * 32)
            with self.assertRaises(subprocess.CalledProcessError):
                real_run("rpmkeys", "--checksig", str(damaged))
            Path("dist/barista.deb").write_bytes(b"different rerun bytes")
            os.environ["RELEASE_VERSION"] = "0.1.123"
            with self.assertRaisesRegex(ValueError, "immutable"):
                repositories.archive(list(records.values()))
            # Publish a second build and upgrade through authenticated APT
            # metadata into an isolated dpkg root. Stable stays on the promoted
            # build while Preview advances.
            (deb / "control").write_text((deb / "control").read_text().replace("0.1.123", "0.1.124"))
            real_run("dpkg-deb", "--root-owner-group", "--build", "deb", "dist/barista.deb")
            spec.write_text(spec.read_text().replace("0.1.123", "0.1.124"))
            real_run("rpmbuild", "--define", "_topdir " + str(rpmroot), "-bb", str(spec))
            shutil.copy2(next((rpmroot / "RPMS").rglob("barista-0.1.124-*.rpm")), dist / "barista.rpm")
            (arch / ".PKGINFO").write_text((arch / ".PKGINFO").read_text().replace("0.1.123", "0.1.124"))
            real_run("tar", "--zstd", "-cf", "dist/barista.pkg.tar.zst", "-C", "arch", ".PKGINFO")
            os.environ["RELEASE_VERSION"] = "0.1.124"
            shutil.rmtree("site")
            repositories.prepare()
            notices = json.loads(Path("site/updates/v1.json").read_text())
            self.assertEqual(notices["channels"]["preview"]["version"], "0.1.124")
            self.assertEqual(notices["channels"]["stable"]["version"], "0.1.123")
            install_deb("0.1.124")
            # APT refuses altered signed metadata (rather than accepting an
            # unsigned fallback); native RPM verification rejected damage above.
            inrelease = Path("site/apt/ubuntu/noble/dists/preview/InRelease")
            inrelease.write_text(inrelease.read_text().replace("Codename: preview", "Codename: altered"))
            with self.assertRaises(subprocess.CalledProcessError):
                real_run(*apt_args, "-o", "APT::Update::Error-Mode=any", "update")


if __name__ == "__main__":
    unittest.main()
