# Continuous integration

[Back to README](../README.md)

The **Continuous** workflow runs for pull requests, pushes to `main`, merge
queues, and manual dispatches. PR jobs have read-only repository permissions,
do not receive release credentials, and do not publish packages to a release.
A newer commit cancels the previous run for the same PR.

## Checks

- **Documentation and CI checks** validates local inline Markdown links/images,
  runs CI helper tests, and lints the workflow and shell syntax. It does not
  fetch external links or validate heading anchors.
- Markdown-only changes (plus images under `docs/screenshots`) skip builds.
  Unknown history and manual/merge-queue runs use the full pipeline.
- Code changes run Linux Debug tests, a separate UI/core-only build, and
  Debian, Fedora RPM, and Arch Release builds with CTest.
  Fedora builds a pinned, minimal upstream FFmpeg for tests only. Its packaged
  OpenH264 decoder crops frames to 854×480 even with cropping disabled, while
  reconstruction checks require all 864×480 coded pixels. The tests explicitly
  select FFmpeg's native `h264` decoder; neither checks nor padding comparisons
  are skipped. This reference decoder is not shipped in Barista packages.
- Each package is installed using its native package manager in a fresh runtime
  container. Checks cover dependency resolution, dynamic libraries, privileged
  binary ownership/permissions, D-Bus/systemd installation, desktop entries, and
  GUI smoke tests as a regular user. Containers do not boot the system service
  or test actual Wi-Fi pairing or adapter cleanup.
- Failed builds retain CTest/configuration logs for 14 days. Installation
  failures retain the container output. Jobs and tests have explicit timeouts.
- Signed repository integration uses disposable keys and fake GitHub storage
  with real APT/RPM/pacman repository tools. It checks signatures, archive
  immutability, byte-identical promotion, interrupted-promotion recovery, and
  signed APT installation/upgrades into an isolated package root. No production
  credentials or publication permissions are needed for this check.

## Required check on main

Configure the repository's branch protection or ruleset to require **CI required**
for `main`. It reports a result even on docs-only PRs and fails if any applicable
job fails, is cancelled, or is unexpectedly skipped. Do not require individual
conditional package jobs instead of this aggregate check.

After the workflow has run once, select **CI required** in GitHub's required
status checks. Keep any existing review/security protections. This repository
file does not itself configure GitHub branch protection.

## Releases and update repositories

Continuous assigns one `major.minor.build` version to all three packages. Only
a successful full pipeline on a code-changing push to `main` can supply new
packages to the protected repository publisher, and only after the maintainer
enables it. PRs, docs-only pushes, and manual validation runs do not publish.

The publisher signs and archives Preview packages, assembles signed APT/RPM/
pacman repositories, and deploys GitHub Pages. A separate manual operation
promotes an existing Preview to Stable without rebuilding. Weekly runs refresh
metadata. Publication and promotion share one concurrency lock.

The old `continuous` release remains untouched as a historical download. It is
no longer updated or used as an update feed. See [release administration](releasing.md)
for signing secrets, Pages setup, immutable archives, retention, and recovery.

## Local checks

```sh
python3 .github/scripts/check_docs.py
python3 -m unittest discover -s .github/scripts -p 'test_*.py'
```

The release helper tests use a fake GitHub CLI; they never publish anything.
Package scripts are intended only for disposable CI containers, not your desktop.
