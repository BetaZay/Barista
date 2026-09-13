#!/usr/bin/env bash
# Called only by the serialized, main-only publishing job.
set -euo pipefail
: "${GH_REPO:?}" "${SHA:?}" "${GITHUB_RUN_ID:?}" "${GITHUB_RUN_ATTEMPT:?}"

head_sha=$(gh api "repos/$GH_REPO/git/ref/heads/main" --jq .object.sha)
if [[ "$head_sha" != "$SHA" ]]; then
    echo "Skipping stale build $SHA; main is now $head_sha"
    exit 0
fi

shopt -s nullglob
packages=(dist/*.deb dist/*.rpm dist/*.pkg.tar.zst)
if (( ${#packages[@]} != 3 )); then
    echo "Expected exactly three tested packages" >&2
    exit 1
fi

# Upload uniquely named files first. Neither the release nor its working assets
# are removed if an upload fails. Names also identify the tested source commit.
staging=$(mktemp -d)
assets=()
for package in "${packages[@]}"; do
    filename=$(basename "$package")
    asset="$staging/barista-${SHA:0:12}-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}-${filename#barista-}"
    cp "$package" "$asset"
    assets+=("$asset")
done

release_id=$(gh api --paginate "repos/$GH_REPO/releases" \
    --jq '.[] | select(.tag_name == "continuous") | .id')
old_assets=()
if [[ -n "$release_id" ]]; then
    # Only old package assets are pruned; leave other release attachments alone.
    old_asset_ids=$(gh api --paginate "repos/$GH_REPO/releases/$release_id/assets" \
        --jq '.[] | select(.name | test("^barista-.*\\.(deb|rpm|pkg\\.tar\\.zst)$")) | .id')
    if [[ -n "$old_asset_ids" ]]; then mapfile -t old_assets <<< "$old_asset_ids"; fi
else
    gh release create continuous --target "$SHA" --title Continuous --prerelease --draft
fi

gh release upload continuous "${assets[@]}"

# Recheck after uploads: main may have moved while large assets were uploading.
head_sha=$(gh api "repos/$GH_REPO/git/ref/heads/main" --jq .object.sha)
if [[ "$head_sha" != "$SHA" ]]; then
    echo "Main advanced during upload; keeping the existing release metadata and assets"
    exit 0
fi

tag_ref=$(gh api "repos/$GH_REPO/git/matching-refs/tags/continuous" \
    --jq '.[] | select(.ref == "refs/tags/continuous") | .ref')
if [[ -n "$tag_ref" ]]; then
    gh api --method PATCH "repos/$GH_REPO/git/refs/tags/continuous" -f sha="$SHA" -F force=true
else
    gh api --method POST "repos/$GH_REPO/git/refs" -f ref=refs/tags/continuous -f sha="$SHA"
fi
gh release edit continuous --draft=false --prerelease --title Continuous \
    --notes "Automated tested packages from $SHA. Asset names include the source commit."

for asset_id in "${old_assets[@]}"; do
    gh api --method DELETE "repos/$GH_REPO/releases/assets/$asset_id"
done
