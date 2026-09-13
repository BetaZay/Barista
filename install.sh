#!/bin/sh

set -eu

repository_url="https://betazay.github.io/Barista"
signing_fingerprint="F8006F0E1A05DF681278EE055E51F16759FC98B9"
channel="${1:-stable}"

fail()
{
    printf 'Barista installer: %s\n' "$*" >&2
    exit 1
}

if [ "$channel" != "stable" ] && [ "$channel" != "preview" ]; then
    fail "channel must be 'stable' or 'preview'"
fi

if [ "$(id -u)" -ne 0 ]; then
    fail "run this installer as root (for example, pipe it to sudo sh)"
fi

if [ "$(uname -m)" != "x86_64" ]; then
    fail "only x86_64 is currently supported"
fi

if [ ! -r /etc/os-release ]; then
    fail "cannot identify this Linux distribution"
fi

# /etc/os-release is supplied by the installed operating system.
# shellcheck disable=SC1091
. /etc/os-release

id_like_contains()
{
    case " ${ID_LIKE:-} " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "${ID:-}" = "arch" ] || id_like_contains arch; then
    command -v pacman >/dev/null 2>&1 || fail "this Arch-based system does not provide pacman"
    package_manager="pacman"
elif [ "${ID:-}" = "ubuntu" ] || id_like_contains ubuntu; then
    ubuntu_codename="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
    if [ "${VERSION_ID:-}" != "24.04" ] && [ "$ubuntu_codename" != "noble" ]; then
        fail "only Ubuntu 24.04 (Noble) and derivatives based on it are currently supported"
    fi
    command -v apt-get >/dev/null 2>&1 || fail "this Ubuntu-based system does not provide apt-get"
    package_manager="apt"
elif [ "${ID:-}" = "fedora" ] || id_like_contains fedora; then
    [ "${VERSION_ID:-}" = "44" ] || fail "only Fedora 44 and derivatives based on it are currently supported"
    command -v dnf >/dev/null 2>&1 || fail "this Fedora-based system does not provide dnf"
    package_manager="dnf"
else
    fail "supported systems are Ubuntu 24.04, Fedora 44, Arch Linux, and compatible derivatives"
fi

printf 'Installing Barista from the %s channel on %s...\n' "$channel" "${PRETTY_NAME:-${ID}}"

case "$package_manager" in
    apt)
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y ca-certificates curl gnupg
        ;;
    dnf)
        dnf install -y ca-certificates curl gnupg2
        ;;
    pacman)
        pacman -Syu --needed --noconfirm ca-certificates curl gnupg
        ;;
esac

temporary_directory="$(mktemp -d)"
cleanup()
{
    rm -rf "$temporary_directory"
}
trap cleanup EXIT HUP INT TERM

key_file="$temporary_directory/barista.asc"
gpg_home="$temporary_directory/gnupg"
install -d -m 700 "$gpg_home"
curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location \
    --output "$key_file" "$repository_url/barista.asc"

actual_fingerprint="$(gpg --batch --homedir "$gpg_home" --show-keys --with-colons "$key_file" \
    | awk -F: '$1 == "fpr" { print $10; exit }')"
if [ "$actual_fingerprint" != "$signing_fingerprint" ]; then
    fail "the downloaded repository key has an unexpected fingerprint"
fi

case "$package_manager" in
    apt)
        install -d -m 755 /etc/apt/keyrings
        install -m 644 "$key_file" /etc/apt/keyrings/barista.asc
        sources_file="$temporary_directory/barista.sources"
        printf '%s\n' \
            'Types: deb' \
            "URIs: $repository_url/apt/ubuntu/noble" \
            "Suites: $channel" \
            'Components: main' \
            'Architectures: amd64' \
            'Signed-By: /etc/apt/keyrings/barista.asc' > "$sources_file"
        install -m 644 "$sources_file" /etc/apt/sources.list.d/barista.sources
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y barista
        ;;
    dnf)
        install -d -m 755 /etc/pki/rpm-gpg
        install -m 644 "$key_file" /etc/pki/rpm-gpg/RPM-GPG-KEY-barista
        rpm --import /etc/pki/rpm-gpg/RPM-GPG-KEY-barista
        repo_file="$temporary_directory/barista.repo"
        printf '%s\n' \
            '[barista]' \
            "name=Barista $channel" \
            "baseurl=$repository_url/rpm/fedora/44/$channel/x86_64" \
            'enabled=1' \
            'gpgcheck=1' \
            'repo_gpgcheck=1' \
            'gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-barista' > "$repo_file"
        install -m 644 "$repo_file" /etc/yum.repos.d/barista.repo
        dnf install -y barista
        ;;
    pacman)
        if grep -Eq '^[[:space:]]*\[barista\][[:space:]]*$' /etc/pacman.conf; then
            fail "an existing [barista] section in /etc/pacman.conf must be removed before using this installer"
        fi
        pacman-key --add "$key_file"
        pacman-key --lsign-key "$signing_fingerprint"
        install -d -m 755 /etc/pacman.d
        repo_file="$temporary_directory/barista.conf"
        printf '%s\n' \
            '[barista]' \
            'SigLevel = Required DatabaseRequired' \
            "Server = $repository_url/arch/$channel/x86_64" > "$repo_file"
        install -m 644 "$repo_file" /etc/pacman.d/barista.conf
        if ! grep -Fqx 'Include = /etc/pacman.d/barista.conf' /etc/pacman.conf; then
            printf '\nInclude = /etc/pacman.d/barista.conf\n' >> /etc/pacman.conf
        fi
        pacman -Syu --needed --noconfirm barista
        ;;
esac

install -d -m 755 /etc/barista
printf '%s\n' "$channel" > /etc/barista/update-channel
chmod 644 /etc/barista/update-channel

printf 'Barista is installed and subscribed to the %s channel.\n' "$channel"
printf 'Open Barista from your application menu as your normal desktop user.\n'
