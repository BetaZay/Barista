#!/usr/bin/env bash
# Fresh runtime containers deliberately have no compiler or Qt development packages.
set -euo pipefail
case "$1" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y /packages/*.deb systemd desktop-file-utils
        ;;
    rpm)
        dnf -y install /packages/*.rpm systemd desktop-file-utils shadow-utils util-linux
        ;;
    arch)
        pacman -Syu --noconfirm --needed systemd desktop-file-utils shadow util-linux
        pacman -U --noconfirm /packages/*.pkg.tar.zst
        ;;
    *) echo "Unknown package type: $1" >&2; exit 1 ;;
esac
bash /checks/check-package.sh
