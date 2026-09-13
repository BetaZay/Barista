# Release and repository administration

## One-time owner setup

1. Enable GitHub Pages with **GitHub Actions** as the source. Protect the
   `github-pages` environment so only `main` can deploy; require approval if
   appropriate. Preserve the existing required CI check and PR protections.
2. Create a dedicated OpenPGP signing identity, retaining the primary secret
   key and revocation certificate offline. Export only an unattended,
   unencrypted **signing subkey**, not the primary secret key, into the protected
   environment secret `BARISTA_SIGNING_KEY`. A dedicated RSA signing subkey is
   suitable for the initial RPM/APT/pacman targets. Never commit private keys.
3. Set repository variable `BARISTA_SIGNING_FINGERPRINT` to the full primary
   fingerprint. Publish that fingerprint independently in the user
   setup documentation/release announcement before users subscribe. The public
   key exported by the workflow is served as `barista.asc`.
4. Set repository variable `BARISTA_REPOSITORIES_ENABLED=true` only after
   credentials and Pages are ready. Until then, CI builds/tests packages but
   intentionally skips publishing. The old `continuous` release remains a
   historical download, not an update feed.
5. Push a code change through normal review and CI, or manually dispatch the
   full Continuous workflow on `main`. Validate the Preview
   repositories on all three target systems, then manually run **Package
   repositories** on `main` with its version in `promote`. A blank value only
   refreshes metadata; the scheduled weekly run does the same.

The signing key must remain available for weekly refreshes. APT metadata expires
after 30 days. Monitor failed scheduled deployments and key expiration. Rotating
keys requires a planned trust transition: keep old public keys available for
archive verification and distribute the new trust root before switching signers.
The initial workflow supports one signing identity; do not simply replace it
without implementing that transition.

## Version assignment

`VERSION` contains major.minor. Continuous assigns its `GITHUB_RUN_NUMBER` as the
third component once and passes it to each package build. PR/docs runs leave
harmless gaps. Reruns retain their number. See GitHub's [variable reference](https://docs.github.com/en/actions/reference/workflows-and-actions/variables).

Do not replace/reset the Continuous workflow counter. Publication rejects lower
or reused counters. If the workflow identity must change, migrate the allocation
scheme explicitly above the highest archived number first. Increase major/minor
in `VERSION` through a reviewed change; never reset the build component.

Local builds default to major.minor.0 and cannot be published. A developer can
pass `-DBARISTA_VERSION=0.1.123` for package tests; that alone does not authorize
publication. Package-manager release revisions remain `1`. Packaging changes
receive a new build number rather than replacing an existing package.

## Archive, promotion, and deployment

Only code-changing main pushes after **CI required** succeeds supply new
packages to the protected reusable publisher. It validates native package
versions, signs packages/metadata, and archives packages plus a signed
`build.json` at `build-major.minor.build`. The manifest contains unsigned input
hashes, final package hashes, the source commit, and originating workflow run.

Published archives are immutable by policy: never overwrite assets or move
their tags. Rerunning an unchanged publisher reuses the archive; if rebuilt
inputs differ it fails rather than replacing the version. An interrupted draft
upload may be recreated because it has never been published. Enable suitable
GitHub tag protections for `build-*` and `v*` without blocking release creation.

Stable promotion selects an existing signed archive and deploys its exact bytes.
Before deployment an immutable `stable-intent-major.minor.build` release records
the approved promotion. Only after Pages reports success is `vmajor.minor.build`
created. If deployment or final tagging fails, the next publication completes
that intent instead of reverting Stable. Protect `stable-intent-*` tags too.
No rebuild or version allocation occurs during promotion.

Both channels share a publication lock. The site is assembled from authenticated
archives, with APT signatures, RPM package/repomd signatures, and Arch package/
database signatures. Pacman database aliases are real files because [Pages
artifacts prohibit symbolic links](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

RPM packages explicitly use the supported v4 format so Fedora's RPM 6 builder
and Ubuntu 24.04's publication/signing tools share a format. [RPM 6 otherwise
defaults to v6 packages](https://rpm.org/releases/6.0.0).

Pages retains the latest ten Preview and three Stable builds, plus builds less
than seven days old. Every build's archive remains on GitHub Releases. If the
site exceeds 900 MiB, publication fails before deployment instead of removing
the grace period or breaking the existing site. Review capacity and move hosting
if needed. The manifest for GUI notices deploys with its corresponding packages.

## Acceptance checks before enabling public updates

- In disposable Ubuntu 24.04, Fedora 44, and Arch systems, install through the
  signed repository, upgrade from one build to the next, and verify the GUI,
  service, and package manager report the same version.
- Confirm bad keys, modified metadata, and modified packages are rejected.
- Promote a Preview and compare package SHA-256 hashes across both channels.
- In booted systemd VMs, test idle and active upgrades, pre-existing masks,
  interrupted transactions, and failure to stop the service. Arch's installed
  [PreTransaction hook](https://man.archlinux.org/man/alpm-hooks.5) uses
  `AbortOnFail`; older installations need the migration precaution.
- On real hardware, confirm graceful disconnect restores the dedicated adapter
  and that saved credentials and settings survive. Containers do not establish
  hardware cleanup or service lifecycle correctness.
