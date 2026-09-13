"""Signed static repositories. Published build-* releases are immutable archives.

Only the protected, serialized repository workflow calls this script. A failed
assembly never replaces Pages. Stable tags are written only after deployment.
"""
import datetime as dt
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

TARGETS = ("ubuntu-24.04-x86_64", "fedora-44-x86_64", "arch-x86_64")
PATTERN = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"


def version(value):
    if not re.fullmatch(PATTERN, value):
        raise ValueError(f"Invalid version: {value!r}")
    result = tuple(map(int, value.split(".")))
    if not result[2]:
        raise ValueError("Development builds cannot be published")
    return result


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def sign(path, clear=False, armor=True):
    output = path.with_name("InRelease") if clear else Path(str(path) + ".asc")
    run("gpg", "--batch", "--yes", "--local-user", os.environ["SIGNING_FINGERPRINT"],
        *(["--armor"] if armor else []), "--output", str(output), "--clearsign" if clear else "--detach-sign", str(path))
    return output


def verify_archive(directory):
    run("gpg", "--batch", "--verify", str(directory / "build.json.asc"), str(directory / "build.json"))
    manifest = json.loads((directory / "build.json").read_text())
    version(manifest["version"])
    if set(manifest["packages"]) != set(TARGETS):
        raise ValueError("Archive is missing a supported target")
    for package in manifest["packages"].values():
        name = package["file"]
        if Path(name).name != name or digest(directory / name) != package["sha256"]:
            raise ValueError("Archive checksum or filename mismatch")
    return manifest


def retained(builds, stable, now):
    ordered = sorted(builds, key=version)
    stable_versions = sorted(set(stable) & set(builds), key=version)
    keep = set(ordered[-10:]) | set(stable_versions[-3:])
    for number, item in builds.items():
        age = now - dt.datetime.fromisoformat(item["published"])
        if age < dt.timedelta(days=7):
            keep.add(number)
    return keep


def releases():
    pages = run("gh", "api", "--paginate", "--slurp", f"repos/{os.environ['GH_REPO']}/releases")
    return [item for page in json.loads(pages) for item in page]


def download(tag, destination):
    destination.mkdir(parents=True, exist_ok=True)
    run("gh", "release", "download", tag, "--dir", str(destination))


def archive(existing):
    number = os.environ.get("RELEASE_VERSION", "")
    if not number:
        return
    assigned = version(number)
    source = os.environ["RELEASE_SOURCE"]
    if not re.fullmatch(r"[0-9a-f]{40}", source):
        raise ValueError("Invalid source revision")
    tag = "build-" + number
    inputs = list(Path("dist").glob("*.deb")) + list(Path("dist").glob("*.rpm")) + list(Path("dist").glob("*.pkg.tar.zst"))
    if len(inputs) != 3 or len({p.suffix for p in inputs}) != 3:
        raise ValueError("Expected exactly three tested packages")
    unsigned = {p.name: digest(p) for p in inputs}
    previous = next((r for r in existing if r["tag_name"] == tag), None)
    if previous and not previous["draft"]:
        destination = Path("archives") / number
        download(tag, destination)
        old = verify_archive(destination)
        if old["source"] != source or old["unsigned"] != unsigned:
            raise ValueError("Published version is immutable; rerun artifacts differ. Run new CI for a new build number.")
        return
    counters = [version(r["tag_name"][6:]) for r in existing
                if re.fullmatch("build-" + PATTERN, r["tag_name"]) and not r["draft"]]
    if counters and (assigned <= max(counters) or assigned[2] <= max(v[2] for v in counters)):
        raise ValueError("Refusing a backward/reused build counter; preserve the Continuous workflow counter")
    destination = Path("archives") / number
    destination.mkdir(parents=True, exist_ok=True)
    packages = {}
    for package, target in zip(inputs, TARGETS):
        suffix = ".deb" if target.startswith("ubuntu") else ".rpm" if target.startswith("fedora") else ".pkg.tar.zst"
        file = destination / ("barista-" + number + "-" + target + suffix)
        shutil.copy2(package, file)
        if suffix == ".deb":
            actual = run("dpkg-deb", "-f", str(file), "Version")
            if actual != number + "-1":
                raise ValueError(f"DEB version mismatch: {actual}")
        elif suffix == ".rpm":
            actual = run("rpm", "-qp", "--qf", "%{VERSION}-%{RELEASE}", str(file))
            if actual != number + "-1":
                raise ValueError(f"RPM version mismatch: {actual}")
            run("rpmsign", "--define", "_gpg_name " + os.environ["SIGNING_FINGERPRINT"],
                "--define", "_gpg_path " + os.environ["GNUPGHOME"],
                "--define", "__gpg /usr/bin/gpg", "--addsign", str(file))
        else:
            actual = run("tar", "-xOf", str(file), ".PKGINFO")
            if f"pkgver = {number}-1\n" not in actual + "\n":
                raise ValueError("Arch version mismatch")
            signature = sign(file, armor=False)
            signature.rename(str(file) + ".sig")
        packages[target] = {"file": file.name, "sha256": digest(file)}
    manifest = {"version": number, "source": source, "run": os.environ["GITHUB_RUN_ID"],
                "published": dt.datetime.now(dt.timezone.utc).isoformat(), "unsigned": unsigned, "packages": packages}
    write_json(destination / "build.json", manifest)
    sign(destination / "build.json")
    if previous:
        # A draft was never published; recover an interrupted upload.
        run("gh", "release", "delete", tag, "--yes")
    run("gh", "release", "create", tag, "--target", source, "--draft", "--prerelease",
        "--title", "Preview " + number, "--notes", f"Tested build {number} from {source}. Install through the Preview repository.")
    run("gh", "release", "upload", tag, *map(str, sorted(destination.iterdir())))
    run("gh", "release", "edit", tag, "--draft=false")


def prepare():
    fingerprint = os.environ["SIGNING_FINGERPRINT"]
    if not re.fullmatch(r"[A-Fa-f0-9]{40}", fingerprint):
        raise ValueError("Set the full signing-key fingerprint")
    archive(releases())
    records = releases()
    builds = {}
    for item in records:
        tag = item["tag_name"]
        if item["draft"] or not re.fullmatch("build-" + PATTERN, tag):
            continue
        number = tag[6:]
        directory = Path("archives") / number
        if not (directory / "build.json").exists():
            directory.mkdir(parents=True, exist_ok=True)
            run("gh", "release", "download", tag, "--pattern", "build.json*", "--dir", str(directory))
        run("gpg", "--batch", "--verify", str(directory / "build.json.asc"), str(directory / "build.json"))
        manifest = json.loads((directory / "build.json").read_text())
        if manifest["version"] != number:
            raise ValueError("Archive tag/version mismatch")
        builds[number] = manifest
    if not builds:
        raise ValueError("No published Preview builds exist")
    stable = [r["tag_name"][1:] for r in records if not r["draft"] and re.fullmatch("v" + PATTERN, r["tag_name"])]
    # Persist promotion intent before deployment. If final tagging fails, the
    # next publication completes the authorized promotion instead of reverting.
    stable += [r["tag_name"][14:] for r in records if not r["draft"] and re.fullmatch("stable-intent-" + PATTERN, r["tag_name"])]
    promote = os.environ.get("PROMOTE_VERSION", "")
    if promote:
        version(promote)
        if promote not in builds:
            raise ValueError("Only an archived tested Preview may be promoted")
        if stable and version(promote) < max(map(version, stable)):
            raise ValueError("Stable cannot move backward")
        if promote not in stable:
            stable.append(promote)
    now = dt.datetime.now(dt.timezone.utc)
    keep = retained(builds, stable, now)
    for number in keep:
        directory = Path("archives") / number
        run("gh", "release", "download", "build-" + number, "--dir", str(directory), "--clobber")
        verify_archive(directory)
    site = Path("site")
    site.mkdir(exist_ok=False)
    (site / "barista.asc").write_text(run("gpg", "--armor", "--export", fingerprint) + "\n")
    (site / "index.html").write_text('<!doctype html><title>Barista updates</title><h1>Barista package repositories</h1><p>See <a href="https://github.com/BetaZay/Barista/blob/main/docs/updates.md">setup instructions</a>.</p>')
    channels = {"preview": sorted(keep, key=version)}
    if stable:
        channels["stable"] = sorted(set(stable) & keep, key=version)
    notice = {"schema": 1, "channels": {}}
    for channel, numbers in channels.items():
        latest = numbers[-1]
        notice["channels"][channel] = {"version": latest, "targets": list(TARGETS)}
        apt = site / "apt/ubuntu/noble"
        pool = apt / "pool" / channel
        rpm = site / "rpm/fedora/44" / channel / "x86_64"
        arch = site / "arch" / channel / "x86_64"
        for directory in (pool, rpm, arch):
            directory.mkdir(parents=True, exist_ok=True)
        for number in numbers:
            for target, directory in zip(TARGETS, (pool, rpm, arch)):
                file = Path("archives") / number / builds[number]["packages"][target]["file"]
                shutil.copy2(file, directory / file.name)
                if target.startswith("arch"):
                    run("gpg", "--batch", "--verify", str(file) + ".sig", str(file))
                    shutil.copy2(str(file) + ".sig", directory / (file.name + ".sig"))
        index = apt / "dists" / channel / "main/binary-amd64"
        index.mkdir(parents=True)
        contents = run("apt-ftparchive", "packages", "pool/" + channel, cwd=apt) + "\n"
        (index / "Packages").write_text(contents)
        (index / "Packages.gz").write_bytes(gzip.compress(contents.encode(), mtime=0))
        suite = apt / "dists" / channel
        expiry = (now + dt.timedelta(days=30)).strftime("%a, %d %b %Y %H:%M:%S +0000")
        release = run("apt-ftparchive", "-o", f"APT::FTPArchive::Release::Suite={channel}",
                      "-o", f"APT::FTPArchive::Release::Codename={channel}",
                      "-o", "APT::FTPArchive::Release::Architectures=amd64",
                      "-o", "APT::FTPArchive::Release::Components=main", "release", ".", cwd=suite)
        (suite / "Release").write_text("Valid-Until: " + expiry + "\n" + release + "\n")
        sign(suite / "Release").rename(suite / "Release.gpg")
        sign(suite / "Release", clear=True)
        run("createrepo_c", str(rpm))
        sign(rpm / "repodata/repomd.xml")
        run("docker", "run", "--rm", "-v", str(arch.resolve()) + ":/repo", "-w", "/repo",
            "archlinux:latest", "bash", "-c", "repo-add barista.db.tar.gz ./*.pkg.tar.zst")
        # Pages forbids symlinks; pacman expects .db and .files aliases.
        for name in ("barista.db", "barista.files"):
            alias = arch / name
            if alias.is_symlink():
                alias.unlink()
            shutil.copy2(arch / (name + ".tar.gz"), alias)
            sign(alias, armor=False).rename(str(alias) + ".sig")
    (site / "updates").mkdir()
    write_json(site / "updates/v1.json", notice)
    if any(p.is_symlink() for p in site.rglob("*")):
        raise ValueError("Pages artifact contains a symlink")
    if sum(p.stat().st_size for p in site.rglob("*") if p.is_file()) > 900 * 1024 * 1024:
        raise ValueError("Repository exceeds 900 MiB; existing Pages deployment is unchanged")
    if promote and not any(r["tag_name"] == "stable-intent-" + promote for r in records):
        run("gh", "release", "create", "stable-intent-" + promote, "--target", builds[promote]["source"],
            "--prerelease", "--title", "Stable promotion intent " + promote,
            "--notes", "Approved Stable promotion. The serialized repository publisher completes deployment and creates the v tag.")
    write_json(Path("promotion.json"), builds[max(stable, key=version)] if stable else {})


def finalize():
    manifest = json.loads(Path("promotion.json").read_text())
    if not manifest:
        return
    tag = "v" + manifest["version"]
    if any(r["tag_name"] == tag and not r["draft"] for r in releases()):
        return
    run("gh", "release", "create", tag, "--target", manifest["source"], "--title", "Barista " + manifest["version"],
        "--notes", "Stable promotion of the exact signed packages archived at build-" + manifest["version"] + ".")


if __name__ == "__main__":
    {"prepare": prepare, "finalize": finalize}[sys.argv[1]]()
