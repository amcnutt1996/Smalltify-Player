#!/bin/bash
# Publish the release artifacts in build/release/ to a GitHub release.
# Build them first with packaging/macos/release.sh, packaging/windows/release.sh
# and packaging/linux/release.sh; see DECISIONS.md #49 and #54.
#
#   packaging/publish.sh <version> [--notes-file FILE] [--dry-run]
#
# Uploads whichever of these exist (at least one must):
#   build/release/Smalltify-Player-<version>-arm64.dmg
#   build/release/Smalltify-Player-<version>-windows-x64.zip
#   build/release/Smalltify-Player-<version>-linux-x64.tar.gz
# plus build/release/SHA256SUMS-<version>.txt, written here for exactly those.
#
# No release <version> yet: creates it as a pre-release titled <version>,
# tagging <version> on the current main HEAD (which must be pushed);
# --notes-file is then required. Release exists: replaces same-named assets
# (gh release upload --clobber) and, with --notes-file, replaces the notes.
#
# --dry-run writes the checksum file and prints the gh commands without
# running them; nothing is uploaded, created or changed on GitHub.
#
# Needs git and an authenticated gh (`gh auth login`).
set -euo pipefail

die() {
  printf 'publish.sh: %s\n' "$*" >&2
  exit 1
}

warn() {
  printf 'publish.sh: warning: %s\n' "$*" >&2
}

usage="usage: publish.sh <version> [--notes-file FILE] [--dry-run]"
version=""
notes=""
dry_run=0
while [ $# -gt 0 ]; do
  case "$1" in
    --notes-file)
      [ $# -ge 2 ] || die "--notes-file needs a file ($usage)"
      notes="$2"
      shift 2
      ;;
    --dry-run) dry_run=1; shift ;;
    -*) die "unknown argument: $1 ($usage)" ;;
    *)
      [ -z "$version" ] || die "more than one version given ($usage)"
      version="$1"
      shift
      ;;
  esac
done
[ -n "$version" ] || die "$usage"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
  die "version '$version' is not of the form x.y.z"
if [ -n "$notes" ]; then
  [ -f "$notes" ] || die "notes file not found: $notes"
  notes="$(cd "$(dirname "$notes")" && pwd)/$(basename "$notes")"
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/release"
cd "$root"

# The artifacts are named after the CMake version, so a mismatch usually
# means a typo or a stale build. Warn only: re-publishing an older version
# from a newer tree is legitimate.
cmake_version="$(sed -n 's/^project(smalltify_player VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ -n "$cmake_version" ] || die "no project(smalltify_player VERSION ...) in CMakeLists.txt"
if [ "$version" != "$cmake_version" ]; then
  warn "version $version differs from CMakeLists.txt ($cmake_version)"
fi

# --- artifacts ------------------------------------------------------------
assets=()
for name in "Smalltify-Player-$version-arm64.dmg" \
            "Smalltify-Player-$version-windows-x64.zip" \
            "Smalltify-Player-$version-linux-x64.tar.gz"; do
  if [ -f "$out/$name" ]; then
    assets+=("$name")
  else
    echo "==> Not found, skipped: build/release/$name"
  fi
done
[ ${#assets[@]} -gt 0 ] || die "no artifacts for $version in $out"

# Same format from either tool: "<hash>  <name>", checkable with
# `shasum -a 256 -c` or `sha256sum -c` in build/release/.
if command -v sha256sum >/dev/null 2>&1; then
  sha256() { sha256sum "$@"; }
elif command -v shasum >/dev/null 2>&1; then
  sha256() { shasum -a 256 "$@"; }
else
  die "neither sha256sum nor shasum found"
fi
sums="SHA256SUMS-$version.txt"
echo "==> Writing build/release/$sums"
(cd "$out" && sha256 "${assets[@]}") > "$out/$sums.tmp"
mv "$out/$sums.tmp" "$out/$sums"
cat "$out/$sums"
assets+=("$sums")

# --- GitHub ---------------------------------------------------------------
# run <cmd...>: print the command, then run it unless --dry-run.
run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  [ "$dry_run" = 1 ] || "$@"
}

# Read-only queries still run under --dry-run when gh is usable, so the
# printed plan (create or upload) matches what a real run would do.
gh_ok=0
if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
  gh_ok=1
elif [ "$dry_run" = 1 ]; then
  warn "gh is missing or not logged in; assuming release $version does not exist yet"
else
  command -v gh >/dev/null 2>&1 || die "gh not found (https://cli.github.com)"
  die "gh is not logged in; run: gh auth login"
fi

if [ "$gh_ok" = 1 ]; then
  account="$(gh api user --jq .login)" || die "gh could not read the active account"
  repo="$(gh repo view --json nameWithOwner --jq .nameWithOwner)" ||
    die "gh could not resolve the GitHub repository of $root"
  echo "==> gh account: $account, repository: $repo"
fi

exists=0
if [ "$gh_ok" = 1 ] && gh release view "$version" >/dev/null 2>&1; then
  exists=1
fi

files=()
for name in "${assets[@]}"; do
  files+=("$out/$name")
done

if [ "$exists" = 1 ]; then
  echo "==> Release $version exists; uploading with --clobber"
  run gh release upload "$version" "${files[@]}" --clobber
  if [ -n "$notes" ]; then
    run gh release edit "$version" --notes-file "$notes"
  fi
else
  [ -n "$notes" ] || die "release $version does not exist yet; --notes-file is required to create it"
  # Tag the commit GitHub has as main, and make sure it is the local main:
  # the artifacts should come from a commit others can check out.
  head="$(git rev-parse --verify 'main^{commit}')" || die "no local main branch"
  remote_head="$(git ls-remote origin refs/heads/main | cut -f1)"
  if [ "$head" != "$remote_head" ]; then
    msg="local main ($head) is not origin's main (${remote_head:-missing}); push main first"
    if [ "$dry_run" = 1 ]; then warn "$msg"; else die "$msg"; fi
  fi
  [ "$(git rev-parse --abbrev-ref HEAD)" = main ] ||
    warn "the checkout is not on main; the tag goes on main ($head)"
  echo "==> Creating pre-release $version (tag $version on main $head)"
  run gh release create "$version" "${files[@]}" --prerelease \
    --title "$version" --target "$head" --notes-file "$notes"
fi

if [ "$dry_run" = 1 ]; then
  echo "==> Dry run: nothing uploaded"
  exit 0
fi

# --- verify ---------------------------------------------------------------
echo "==> Verifying the release assets"
listed="$(gh release view "$version" --json assets --jq '.assets[].name')"
for name in "${assets[@]}"; do
  grep -qxF "$name" <<<"$listed" || die "asset missing from release $version: $name"
done

echo "==> Done: $(gh release view "$version" --json url --jq .url)"
