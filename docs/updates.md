# Installing and updating Barista

[Back to README](../README.md)

Barista uses `major.minor.build` versions, for example `0.1.147`. The build
counter increases across minor and major releases. Stable and Preview are
separate update channels; Stable promotions contain exactly the same packages
as the selected tested Preview.

## Availability

The signed Preview repositories are live. Stable becomes available when a
tested Preview build is promoted. Do not import an unverified key or disable
signature checks to work around an unavailable repository.

Initial package repositories support **Ubuntu 24.04, Fedora 44, Arch Linux, and
compatible derivatives, x86_64 only**. The installer recognizes derivatives
through `/etc/os-release`; for example, CachyOS uses the Arch package. Ubuntu
packages are not a promise of Debian compatibility.

## Subscribe once

The installer detects the supported distribution, verifies the repository key's
full fingerprint, configures the native package manager, installs Barista, and
records the selected update channel.

Preview currently contains the latest tested build:

```sh
curl -fsSL https://betazay.github.io/Barista/install.sh | sudo sh -s -- preview
```

After a build has been promoted, use Stable instead:

```sh
curl -fsSL https://betazay.github.io/Barista/install.sh | sudo sh -s -- stable
```

The script runs as root because package-manager and repository configuration
require administrator access. You can inspect [install.sh](../install.sh) before
running it, or follow the manual steps below instead.

## Manual setup

Download `barista.asc` from <https://betazay.github.io/Barista/barista.asc>.
Inspect it with `gpg --show-keys --with-fingerprint barista.asc` and compare the
full primary-key fingerprint with Barista's published fingerprint:

```text
F800 6F0E 1A05 DF68 1278  EE05 5E51 F167 59FC 98B9
```

Keep signature verification enabled. These setup steps require
administrator privileges; the Barista GUI never does.

Choose `preview` until the first Stable promotion. After that, choose `stable`
or replace **every** occurrence with `preview`. Subscribe to only one channel.
Both use the same `barista` package name.

### Ubuntu 24.04

Install the verified ASCII public key as `/etc/apt/keyrings/barista.asc`, readable
by all users. Create `/etc/apt/sources.list.d/barista.sources`:

```text
Types: deb
URIs: https://betazay.github.io/Barista/apt/ubuntu/noble
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/barista.asc
```

Then run `sudo apt update` and `sudo apt install barista`.

### Fedora 44

Install the verified key as `/etc/pki/rpm-gpg/RPM-GPG-KEY-barista`, then create
`/etc/yum.repos.d/barista.repo`:

```ini
[barista]
name=Barista Stable
baseurl=https://betazay.github.io/Barista/rpm/fedora/44/stable/x86_64
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-barista
```

Run `sudo dnf install barista`. Check the fingerprint again if DNF prompts to
import the key.

### Arch Linux

Import and locally trust only the verified Barista key:

```sh
sudo pacman-key --add barista.asc
sudo pacman-key --lsign-key F8006F0E1A05DF681278EE055E51F16759FC98B9
```

Add this section to `/etc/pacman.conf`:

```ini
[barista]
SigLevel = Required DatabaseRequired
Server = https://betazay.github.io/Barista/arch/stable/x86_64
```

Run `sudo pacman -Syu barista`. Use full system upgrades on Arch.

### Record the channel for notices

Create `/etc/barista/update-channel` containing just `stable` (or `preview`),
owned by root and readable by users. This records your repository subscription;
it does **not** configure the package manager by itself. Keep it in sync when
changing repository channels. Restart Barista after initial setup.

## Everyday updates

Use your normal system updater, or `sudo apt upgrade`, `sudo dnf upgrade`, or
`sudo pacman -Syu`. No repeated download or source rebuild is needed.

Barista checks once daily and shows a notice when your subscribed channel has
a newer build for your distribution. Settings → About includes a manual check
and a daily-check opt-out. Checks fetch a small public HTTPS manifest from
GitHub Pages; no application identifiers or analytics are sent. GitHub still
receives normal connection information such as your IP address.

An upgrade gracefully disconnects the GamePad and releases the Wi-Fi adapter
before files are replaced. Restart the GUI afterward. Pairing credentials and
settings remain intact; a session is never automatically reconnected by package
installation. Update notices do not grant installation privileges.

Returning from Preview to Stable does not automatically downgrade a newer
installed version. You can wait for Stable to catch up. Explicit downgrades are
an advanced operation and may require matching configuration compatibility.

## Migrating existing installations

For an existing downloaded package, quit Barista, stop its service once, add the
repository, and perform a normal package upgrade. The first repository-enabled
upgrade from older packages needs this precaution because older GUIs and Arch
packages do not contain the new upgrade protections.

For a source installation, check `type -a barista` first. A binary in
`/usr/local/bin` can shadow `/usr/bin/barista`. Stop the old service and use the
original build's `install_manifest.txt` to identify its installed files. Remove
only reviewed source-installed files, then install the package. Do not remove
`/var/lib/barista` or your user settings. Barista will not delete source installs
automatically.

## Interrupted upgrades

If a package transaction fails, first finish or repair it with your distribution's
package manager. A temporary runtime service mask may remain to prevent starting
half-installed binaries. If `/run/barista-package-upgrade` remains **and no package
transaction is running**, after repairing installation run:

```sh
sudo systemctl unmask --runtime barista.service
sudo rmdir /run/barista-package-upgrade
sudo systemctl daemon-reload
```

Do not remove an administrator's unrelated service mask. Open Barista normally
afterward. Use Settings → Support if service shutdown or adapter cleanup failed.
