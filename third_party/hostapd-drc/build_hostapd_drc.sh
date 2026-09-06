#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 7 ]]; then
	echo "usage: $0 <src-dir> <repo-url> <tag> <patch-dir> <hostapd-config> <out-hostapd> <out-hostapd-cli>" >&2
	exit 2
fi

src_dir="$1"
repo_url="$2"
tag="$3"
patch_dir="$4"
hostapd_config="$5"
out_hostapd="$6"
out_hostapd_cli="$7"

if [[ ! -d "$src_dir/.git" ]]; then
	mkdir -p "$(dirname "$src_dir")"
	# The source is pinned by commit, not a branch name. Clone first, then reset
	# below; `git clone --branch <commit>` is not portable across Git versions.
	git clone "$repo_url" "$src_dir"
fi

patch_hash="$({ printf '%s\0' "$tag"; sha256sum "$hostapd_config" "$patch_dir"/*.patch; } | sha256sum | cut -d' ' -f1)"
patch_marker="$src_dir/.barista-patchset"
current_hash=""
if [[ -f "$patch_marker" ]]; then
	current_hash="$(<"$patch_marker")"
fi

if [[ "$current_hash" != "$patch_hash" ]]; then
	git -C "$src_dir" am --abort >/dev/null 2>&1 || true
	git -C "$src_dir" reset --hard "$tag"
	git -C "$src_dir" clean -xfd
	# `git am` creates local commits. CI runners do not necessarily have a
	# global identity, so keep this deterministic and scoped to this checkout.
	git -C "$src_dir" config user.name "Barista build"
	git -C "$src_dir" config user.email "build@barista.invalid"
	for patch in "$patch_dir"/*.patch; do
		git -C "$src_dir" am "$patch"
	done
	cp "$hostapd_config" "$src_dir/hostapd/.config"
	printf '%s\n' "$patch_hash" > "$patch_marker"
elif ! cmp -s "$hostapd_config" "$src_dir/hostapd/.config"; then
	cp "$hostapd_config" "$src_dir/hostapd/.config"
fi

make -C "$src_dir/hostapd" -j"$(nproc)" hostapd hostapd_cli

mkdir -p "$(dirname "$out_hostapd")" "$(dirname "$out_hostapd_cli")"
install -m 0755 "$src_dir/hostapd/hostapd" "$out_hostapd"
install -m 0755 "$src_dir/hostapd/hostapd_cli" "$out_hostapd_cli"
