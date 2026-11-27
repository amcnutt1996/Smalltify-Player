#!/bin/bash
# Build and verify the Linux x64 release archive inside Docker
# (packaging/linux/Dockerfile: Ubuntu 22.04, so glibc 2.35 or newer). Needs
# only Docker; runs on macOS, Linux and Windows (Git Bash or WSL) hosts.
#
#   packaging/linux/release.sh
#
# The user's machine needs glibc >= 2.35, libcurl4, libx11-6, a Vulkan loader
# (libvulkan1) and a working Vulkan driver (Mesa or the GPU vendor's).
#
# Output: build/release/Smalltify-Player-<version>-linux-x64.tar.gz, one
# folder Smalltify-Player-<version>-linux-x64/ holding smalltify-player,
# .env (= .env.example), LICENSE and the notices of what the binary embeds:
# Inter-LICENSE.txt, Raven-LICENSE.txt, Raven-NOTICE.txt, cJSON-LICENSE.txt.
set -euo pipefail

die() {
  printf 'release.sh: %s\n' "$*" >&2
  exit 1
}

[ $# -eq 0 ] || die "unknown argument: $1 (usage: release.sh)"

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/build/release"
image=smalltify-build-linux

version="$(sed -n 's/^project(smalltify_player VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[ -n "$version" ] || die "cannot read the version from CMakeLists.txt"
name="Smalltify-Player-$version-linux-x64"
archive="$out/$name.tar.gz"

command -v docker >/dev/null || die "docker not found; install Docker and start it"
docker info >/dev/null 2>&1 || die "docker is installed but not running"

# Git Bash would rewrite the container paths below into Windows paths, and
# Docker wants the host path in Windows form.
mount="$root"
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    export MSYS_NO_PATHCONV=1
    mount="$(cygpath -m "$root")"
    ;;
esac

# --- image ----------------------------------------------------------------
echo "==> Building the image $image (Ubuntu 22.04, linux/amd64)"
docker build --platform linux/amd64 -t "$image" "$root/packaging/linux"

# --- build, test, archive (in the container) -------------------------------
# Runs as the invoking user, so build-linux/ and the archive are theirs.
echo "==> Building $name in the container"
rm -rf "$root/build-linux"
mkdir -p "$out"
rm -f "$archive"
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
  -v "$mount:/src" -w /src -e NAME="$name" "$image" bash -c '
set -euo pipefail
build=/src/build-linux

echo "==> Configuring (MinSizeRel, .env = .env.example)"
cmake -S /src -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DSMP_SEED_ENV=OFF
echo "==> Building"
cmake --build "$build"
echo "==> Testing"
ctest --test-dir "$build" --output-on-failure

# Only what ships: no data/, soloist/ or other build leftovers.
stage="$build/stage/$NAME"
mkdir -p "$stage"
cp -p "$build/dist/smalltify-player" "$build/dist/.env" "$build/dist/Inter-LICENSE.txt" "$stage/"
cp -p /src/LICENSE "$stage/"
# Raven (Apache-2.0: license and NOTICE) and cJSON (MIT) are linked in.
cp -p "$build/_deps/raven-src/LICENSE" "$stage/Raven-LICENSE.txt"
cp -p "$build/_deps/raven-src/NOTICE" "$stage/Raven-NOTICE.txt"
cp -p /src/third_party/cjson/LICENSE "$stage/cJSON-LICENSE.txt"

exe="$stage/smalltify-player"
machine="$(readelf -h "$exe" | sed -n "s/^ *Machine: *//p")"
[ "$machine" = "Advanced Micro Devices X86-64" ] ||
  { echo "release.sh: $exe is for \"$machine\", not x86-64" >&2; exit 1; }
echo "==> Run-time libraries (ldd)"
libs="$(ldd "$exe")"
printf "%s\n" "$libs"
case "$libs" in
  *"not found"*) echo "release.sh: unresolved libraries" >&2; exit 1 ;;
esac
glibc="$(objdump -T "$exe" | grep -o "GLIBC_[0-9][0-9.]*" | sort -uV | tail -n 1)"
echo "==> Newest glibc symbol version required: $glibc"
[ "$(printf "%s\n" "$glibc" GLIBC_2.35 | sort -V | tail -n 1)" = GLIBC_2.35 ] ||
  { echo "release.sh: needs $glibc, newer than the glibc 2.35 floor" >&2; exit 1; }
# A CLI command loads every library without opening a window. A copy, so
# the run leaves nothing in the staged folder.
smoke="$(mktemp -d)"
cp -p "$exe" "$stage/.env" "$smoke/"
"$smoke/smalltify-player" --help >/dev/null
rm -rf "$smoke"

archive="/src/build/release/$NAME.tar.gz"
echo "==> Creating $archive"
tar -C "$build/stage" --sort=name --owner=0 --group=0 --numeric-owner -cf - "$NAME" |
  gzip -9n > "$archive"
'

# --- verify the archive users get -----------------------------------------
[ -f "$archive" ] || die "$archive was not created"
expected="$(printf '%s\n' "$name/" "$name/.env" "$name/Inter-LICENSE.txt" "$name/LICENSE" \
  "$name/Raven-LICENSE.txt" "$name/Raven-NOTICE.txt" "$name/cJSON-LICENSE.txt" \
  "$name/smalltify-player" | LC_ALL=C sort)"
actual="$(tar -tzf "$archive" | LC_ALL=C sort)"
[ "$actual" = "$expected" ] || die "unexpected archive contents:
$actual"
tar -xzOf "$archive" "$name/.env" | cmp -s - "$root/.env.example" ||
  die ".env in the archive is not byte-identical to .env.example"
tar -xzOf "$archive" "$name/LICENSE" | cmp -s - "$root/LICENSE" ||
  die "LICENSE in the archive differs from the repo's"
tar -xzOf "$archive" "$name/Inter-LICENSE.txt" | cmp -s - "$root/third_party/inter/LICENSE.txt" ||
  die "Inter-LICENSE.txt in the archive differs from third_party/inter/LICENSE.txt"
tar -xzOf "$archive" "$name/cJSON-LICENSE.txt" | cmp -s - "$root/third_party/cjson/LICENSE" ||
  die "cJSON-LICENSE.txt in the archive differs from third_party/cjson/LICENSE"

echo "==> Done: $archive"
if command -v shasum >/dev/null; then
  shasum -a 256 "$archive"
else
  sha256sum "$archive"
fi
