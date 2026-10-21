#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 7 ]]; then
	echo "usage: $0 <src-dir> <repo-url> <tag> <hostapd-config> <out-hostapd> <out-hostapd-cli> <ht-patch>" >&2
	exit 2
fi

src_dir="$1"
repo_url="$2"
tag="$3"
hostapd_config="$4"
out_hostapd="$5"
out_hostapd_cli="$6"
ht_patch="$7"

if [[ ! -d "$src_dir/.git" ]]; then
	mkdir -p "$(dirname "$src_dir")"
	# The source is pinned by commit, not a branch name. Clone first, then reset
	# below; `git clone --branch <commit>` is not portable across Git versions.
	clone_dir="${src_dir}.clone-tmp"
	rm -rf "$clone_dir"
	cloned=false
	for attempt in 1 2 3 4; do
		if git -c http.version=HTTP/1.1 clone "$repo_url" "$clone_dir"; then
			cloned=true
			break
		fi
		rm -rf "$clone_dir"
		if [[ "$attempt" -lt 4 ]]; then
			delay=$((attempt * 2))
			echo "hostapd clone attempt $attempt failed; retrying in ${delay}s" >&2
			sleep "$delay"
		fi
	done
	if [[ "$cloned" != true ]]; then
		echo "hostapd clone failed after 4 attempts: $repo_url" >&2
		exit 1
	fi
	rm -rf "$src_dir"
	mv "$clone_dir" "$src_dir"
fi

# Existing build trees may contain the previous upstream repository. Fetch the
# requested pin before resetting this disposable, build-owned checkout.
if [[ "$(git -C "$src_dir" remote get-url origin)" != "$repo_url" ]]; then
	git -C "$src_dir" remote set-url origin "$repo_url"
	git -C "$src_dir" fetch origin "$tag"
elif ! git -C "$src_dir" cat-file -e "${tag}^{commit}" 2>/dev/null; then
	git -C "$src_dir" fetch origin "$tag"
fi

patch_hash="$({ printf '%s\0%s\0ht-advertisement\0' "$repo_url" "$tag"; sha256sum "$hostapd_config" "$ht_patch"; } | sha256sum | cut -d' ' -f1)"
patch_marker="$src_dir/.barista-patchset"
current_hash=""
if [[ -f "$patch_marker" ]]; then
	current_hash="$(<"$patch_marker")"
fi

if [[ "$current_hash" != "$patch_hash" ]]; then
	git -C "$src_dir" am --abort >/dev/null 2>&1 || true
	git -C "$src_dir" reset --hard "$tag"
	git -C "$src_dir" clean -xfd
	git -C "$src_dir" apply --check "$ht_patch"
	git -C "$src_dir" apply "$ht_patch"
	cp "$hostapd_config" "$src_dir/hostapd/.config"
	printf '%s\n' "$patch_hash" > "$patch_marker"
elif ! cmp -s "$hostapd_config" "$src_dir/hostapd/.config"; then
	cp "$hostapd_config" "$src_dir/hostapd/.config"
fi

make -C "$src_dir/hostapd" -j"$(nproc)" hostapd hostapd_cli

mkdir -p "$(dirname "$out_hostapd")" "$(dirname "$out_hostapd_cli")"
install -m 0755 "$src_dir/hostapd/hostapd" "$out_hostapd"
install -m 0755 "$src_dir/hostapd/hostapd_cli" "$out_hostapd_cli"
