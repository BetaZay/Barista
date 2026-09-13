#!/usr/bin/env bash
# Run inside a disposable container AFTER native package installation.
set -euo pipefail

test "$(id -u)" = 0
for binary in /usr/bin/barista /usr/libexec/barista/barista-service \
    /usr/libexec/barista/barista-engine /usr/libexec/barista/barista-hostapd; do
    test -x "$binary"
    test "$(stat -c %u "$binary")" = 0
    test -z "$(find "$binary" -perm /022 -print)"
    dependencies=$(ldd "$binary")
    if [[ "$dependencies" == *"not found"* ]]; then
        printf '%s\n' "$dependencies"
        exit 1
    fi
done

test -f /usr/share/dbus-1/system.d/org.barista.Service1.conf
test -f /usr/share/polkit-1/actions/org.barista.manage-session.policy
grep -Fx 'Exec=/usr/libexec/barista/barista-service' /usr/share/dbus-1/system-services/org.barista.Service1.service
grep -Fx 'SystemdService=barista.service' /usr/share/dbus-1/system-services/org.barista.Service1.service
grep -Fx 'ExecStart=/usr/libexec/barista/barista-service' /usr/lib/systemd/system/barista.service
systemd-analyze verify /usr/lib/systemd/system/barista.service
desktop-file-validate /usr/share/applications/org.barista.Barista.desktop

# Never run the desktop GUI as root, even in CI. Smoke mode does not contact
# the privileged service or request a physical Wi-Fi adapter.
useradd --create-home barista-ci
for page in '' --smoke-pairing --smoke-advanced --smoke-settings --smoke-connection --smoke-about; do
    args=(--smoke-test)
    if [[ -n "$page" ]]; then args+=("$page"); fi
    runuser -u barista-ci -- env QT_QPA_PLATFORM=offscreen /usr/bin/barista "${args[@]}"
done
