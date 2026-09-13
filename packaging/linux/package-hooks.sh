#!/bin/sh
# Shared by Debian/RPM maintainer scripts and pacman's .INSTALL.
# A runtime mask closes the D-Bus reactivation race during replacement.
barista_pre() {
    [ -d /run/systemd/system ] || return 0
    command -v systemctl >/dev/null 2>&1 || return 0
    state=$(systemctl is-enabled barista.service 2>/dev/null || :)
    case "$state" in masked|masked-runtime) owned_mask=no ;; *) owned_mask=yes ;; esac
    if [ "$owned_mask" = yes ] && [ ! -d /run/barista-package-upgrade ]; then
        mkdir -m 700 /run/barista-package-upgrade || return 1
        if ! systemctl mask --runtime barista.service; then
            rmdir /run/barista-package-upgrade
            return 1
        fi
    fi
    if ! systemctl stop barista.service; then
        barista_post
        return 1
    fi
    result=$(systemctl show --property=Result --value barista.service)
    if [ "$result" != success ]; then
        echo "Barista did not stop cleanly ($result). Resolve the service failure before upgrading." >&2
        barista_post
        return 1
    fi
}

barista_post() {
    [ -d /run/systemd/system ] || return 0
    if [ -d /run/barista-package-upgrade ]; then
        systemctl unmask --runtime barista.service || return 1
        rmdir /run/barista-package-upgrade || return 1
    fi
    systemctl daemon-reload
    # D-Bus starts the service on demand; never reconnect a GamePad here.
}

pre_upgrade() { barista_pre; }
post_upgrade() { barista_post; }
post_install() { barista_post; }
