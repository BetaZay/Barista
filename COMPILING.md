# Compiling Barista

Barista has two build layers:

- The portable Qt 6 UI/core can build without the GamePad radio engine.
- The full GamePad implementation is currently Linux-only. It builds Barista's
  patched hostapd and native MiniH264/CABAC encoder as part of the normal build.

## Prerequisites

Install a C++20 compiler, CMake 3.20 or newer, Ninja, Git, Python 3, Qt 6 Core/
Widgets/Network/DBus development packages, OpenSSL development headers, and
the `libnl` development packages required by hostapd. On Linux, `pkcheck`,
`modprobe`, and `systemctl` must be available for the full service build.

Package names vary. On Debian/Ubuntu-like distributions the relevant package
families are commonly `build-essential`, `cmake`, `ninja-build`, `git`,
`python3`, `qt6-base-dev`, `libssl-dev`, `libnl-3-dev`, and
`libnl-genl-3-dev`. Install the matching Qt 6 DBus development package if your
distribution splits it out.

The first full build clones the pinned upstream hostapd source and applies the
patches included in this repository, so it needs Internet access. The native
encoder source is bundled under `core/drh/encoder/x264`.

## Development build

From the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The build produces `build/app/barista`, `build/app/barista-service`, `build/app/barista-engine`,
and test tools. A development build deliberately refuses to let the GUI launch
writable binaries with root privileges; install the project before trying a
real radio session.

## UI/core-only build

This is useful for UI work or non-Linux development:

```sh
cmake -S . -B build-desktop -G Ninja -DBARISTA_BUILD_ENGINE=OFF
cmake --build build-desktop --parallel
ctest --test-dir build-desktop --output-on-failure
```

It does not provide Wi-Fi pairing, radio streaming, or a virtual controller.

## Install on Linux

Choose a prefix and use the normal CMake install step:

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBARISTA_BUILD_DESKTOP=ON -DBARISTA_BUILD_ENGINE=ON \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
sudo cmake --install build-release
```

Installation places the desktop file, D-Bus activation service, polkit policy,
root-owned engine, and Barista's patched hostapd together. Open Barista as the
regular desktop user; do not start the GUI with `sudo` or `pkexec`.

This explicitly enables both frontend and backend, even if `build-release` was
previously configured engine-only. Its executables are `app/barista`,
`app/barista-service`, and `app/barista-engine`; the engine has the same name
before and after installation. `drcctl` and `drcd_reencode_replay` remain legacy
diagnostic tools, not the service engine. Old `app/drcd` files in existing build
directories are stale artifacts and should not be installed.

Before testing a GamePad, close tools that own the selected Wi-Fi adapter and
prefer Ethernet or a second adapter for Internet access. Barista will warn
before it takes the adapter over.

## Native packages

Configure a release build with `/usr` as its prefix before invoking CPack:

```sh
cmake -S . -B build-package -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-package --parallel
```

On Debian-family distributions, generate a `.deb` with:

```sh
cpack --config build-package/CPackConfig.cmake -G DEB -B dist
```

On RPM-family distributions, install `rpm-build` and generate an RPM with:

```sh
cpack --config build-package/CPackConfig.cmake -G RPM -B dist
```

On Arch Linux, install the `makedepends` listed in
`packaging/arch/PKGBUILD`, then run as a regular user from the repository root:

```sh
BARISTA_SOURCE_DIR="$PWD" makepkg --force -p packaging/arch/PKGBUILD
```

The GitHub **Continuous** workflow performs these three builds and replaces the
single rolling prerelease after a successful push to `main`.
