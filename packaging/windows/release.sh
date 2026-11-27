#!/bin/bash
# Build, optionally sign, and verify the Windows x64 release zip. The app is
# cross-compiled with MinGW-w64 in a linux/amd64 Docker container
# (packaging/windows/Dockerfile), so any machine with Docker and bash can
# make it: macOS, Linux, or Windows with Git Bash or WSL (an arm64 Linux
# host needs QEMU binfmt for amd64; Docker Desktop and OrbStack have it).
# Runs from any folder. The build itself runs in the container, where this
# script calls itself with --in-container.
#
#   packaging/windows/release.sh
#
# SMP_WIN_SIGN_PFX   Authenticode certificate with its key (.pfx/.p12), any
#                    path: only that file is mounted, read-only, into the
#                    container, where osslsigncode signs the exe (SHA-256,
#                    RFC 3161 timestamp). Unset: the exe is unsigned, and
#                    Windows SmartScreen warns whoever runs it.
# SMP_WIN_SIGN_PASS  the .pfx password. It reaches osslsigncode through the
#                    environment and a pipe, never a command line.
# SMP_WIN_SIGN_TSA   RFC 3161 timestamp server
#                    (default: http://timestamp.digicert.com).
#
# Output: build/release/Smalltify-Player-<version>-windows-x64.zip, one
# folder with smalltify-player.exe (static: no DLLs beside it), .env
# (byte-identical to .env.example), LICENSE and the licenses of what the exe
# contains. Build folder: build-windows/, recreated each run. The tests are
# compiled but not run (that needs Windows).
set -euo pipefail

die() {
  printf 'release.sh: %s\n' "$*" >&2
  exit 1
}

image=smalltify-build-windows
root="$(cd "$(dirname "$0")/../.." && pwd)"
version="$(sed -n 's/^project(smalltify_player VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[ -n "$version" ] || die "no version in the project() line of CMakeLists.txt"
name="Smalltify-Player-$version-windows-x64"
# What the zip's folder holds, besides the folder itself.
files=(smalltify-player.exe .env LICENSE Inter-LICENSE.txt cJSON-LICENSE.txt
  curl-LICENSE.txt Raven-LICENSE.txt Raven-NOTICE.txt winpthreads-LICENSE.txt)
# DLLs every Windows install has. Anything else the exe imports fails the
# build: it would have to ship beside the exe.
system_dlls=(advapi32.dll bcrypt.dll comdlg32.dll crypt32.dll gdi32.dll iphlpapi.dll
  kernel32.dll msvcrt.dll ole32.dll secur32.dll shell32.dll user32.dll winhttp.dll ws2_32.dll)

# --- inside the container (release.sh runs itself there) ----------------
if [ "${1:-}" = "--in-container" ]; then
  build=/src/build-windows
  stage="$build/stage"
  dir="$stage/$name"
  exe="$dir/smalltify-player.exe"
  licenses=/opt/x86_64-w64-mingw32/share/licenses
  zip="/src/build/release/$name.zip"

  echo "==> Configuring build-windows (MinSizeRel, MinGW-w64 x86_64, no developer values in .env)"
  rm -rf "$build"
  cmake -S /src -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/src/packaging/windows/mingw-w64-x86_64.cmake \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DSMP_SEED_ENV=OFF
  echo "==> Building the app and the tests (the tests need Windows to run)"
  cmake --build "$build"

  # Only what ships: no data/, soloist/ or other build leftovers.
  rm -rf "$stage"
  mkdir -p "$dir"
  cp "$build/dist/smalltify-player.exe" "$build/dist/.env" "$build/dist/Inter-LICENSE.txt" "$dir/"
  cp /src/LICENSE "$dir/LICENSE"
  cp /src/third_party/cjson/LICENSE "$dir/cJSON-LICENSE.txt"
  cp "$licenses/curl-LICENSE.txt" "$licenses/winpthreads-LICENSE.txt" "$dir/"
  cp "$build/_deps/raven-src/LICENSE" "$dir/Raven-LICENSE.txt"
  cp "$build/_deps/raven-src/NOTICE" "$dir/Raven-NOTICE.txt"

  cmp -s "$dir/.env" /src/.env.example ||
    die ".env in the zip folder is not byte-identical to .env.example"

  echo "==> Checking smalltify-player.exe"
  kind="$(file -b "$exe")"
  echo "$kind"
  case "$kind" in
    *PE32+*"(GUI)"*x86-64*) ;;
    *) die "smalltify-player.exe is '$kind', expected a PE32+ GUI x86-64 executable" ;;
  esac
  headers="$(x86_64-w64-mingw32-objdump -p "$exe")"
  grep -Eq '^Magic[[:space:]]+020b[[:space:]]+\(PE32\+\)' <<<"$headers" ||
    die "smalltify-player.exe is not PE32+"
  grep -Eq '^Subsystem[[:space:]]+00000002[[:space:]]+\(Windows GUI\)' <<<"$headers" ||
    die "smalltify-player.exe is not a GUI-subsystem program"
  dlls="$(sed -n 's/^[[:space:]]*DLL Name: //p' <<<"$headers")"
  [ -n "$dlls" ] || die "found no DLL imports in smalltify-player.exe (objdump output changed?)"
  echo "Imports: $(tr '\n' ' ' <<<"$dlls")"
  while read -r dll; do
    lower="$(tr '[:upper:]' '[:lower:]' <<<"$dll")"
    [[ " ${system_dlls[*]} " == *" $lower "* ]] ||
      die "smalltify-player.exe imports $dll, which is not a Windows system DLL (link it statically, or if every Windows has it, add it to system_dlls in release.sh)"
  done <<<"$dlls"

  if [ -n "${SMP_WIN_SIGN_PFX:-}" ]; then
    tsa="${SMP_WIN_SIGN_TSA:-http://timestamp.digicert.com}"
    echo "==> Signing smalltify-player.exe (Authenticode SHA-256, timestamp: $tsa)"
    sign=(osslsigncode sign -pkcs12 "$SMP_WIN_SIGN_PFX" -h sha256 -n "Smalltify Player"
      -i https://github.com/amcnutt1996/Smalltify-Player -ts "$tsa" -in "$exe" -out "$exe.signed")
    if [ -n "${SMP_WIN_SIGN_PASS:-}" ]; then
      "${sign[@]}" -readpass <(printf '%s' "$SMP_WIN_SIGN_PASS")
    else
      "${sign[@]}"
    fi
    mv "$exe.signed" "$exe"
    # Whether Windows trusts the certificate is Windows' call: code-signing
    # roots are often missing from Linux CA bundles. What must hold here: the
    # signature covers the file as shipped, and it carries a timestamp.
    report="$(osslsigncode verify -in "$exe" 2>&1)" && chain=1 || chain=0
    digest() { # digest <Current|Calculated>
      sed -n "s/^$1 message digest *: *\([0-9A-Fa-f]*\).*/\1/p" <<<"$report" | head -n 1
    }
    current="$(digest Current)"
    if [ -z "$current" ] || [ "$current" != "$(digest Calculated)" ]; then
      printf '%s\n' "$report" >&2
      die "the signature does not match smalltify-player.exe"
    fi
    if ! grep -q '^[[:space:]]*Timestamp time:' <<<"$report"; then
      printf '%s\n' "$report" >&2
      die "the signature has no timestamp"
    fi
    if [ "$chain" = 1 ]; then
      echo "Signature, timestamp and certificate chain verified"
    else
      echo "Signed digest matches the exe and is timestamped; the container's CA bundle cannot vouch for the certificate chain (Windows decides whether it trusts it)"
    fi
  else
    echo "==> Not signing (SMP_WIN_SIGN_PFX unset): SmartScreen will warn users who run the exe"
  fi

  # --- zip --------------------------------------------------------------
  mkdir -p "$(dirname "$zip")"
  rm -f "$zip"
  echo "==> Creating build/release/$name.zip"
  (cd "$stage" && zip -q -X "$zip" "$name/" "${files[@]/#/$name/}")

  echo "==> Verifying the zip"
  unzip -tq "$zip"
  listed="$(unzip -Z1 "$zip" | LC_ALL=C sort)"
  expected="$(printf '%s\n' "$name/" "${files[@]/#/$name/}" | LC_ALL=C sort)"
  if [ "$listed" != "$expected" ]; then
    printf 'zip holds:\n%s\nexpected:\n%s\n' "$listed" "$expected" >&2
    die "the zip does not hold exactly the expected files"
  fi
  unzip -p "$zip" "$name/.env" | cmp -s - /src/.env.example ||
    die ".env in the zip is not byte-identical to .env.example"
  for f in "${files[@]}"; do
    unzip -p "$zip" "$name/$f" | cmp -s - "$dir/$f" || die "$f in the zip differs from the checked one"
  done
  exit 0
fi

[ $# -eq 0 ] || die "unknown argument: $1 (usage: release.sh)"

# --- on the host ----------------------------------------------------------
command -v docker >/dev/null || die "docker not found; install Docker (Desktop, Engine or OrbStack)"
docker version >/dev/null 2>&1 ||
  die "cannot reach the Docker daemon (is it running, and may $(id -un) use it?)"

# Docker on Windows takes C:/... paths, and Git Bash would rewrite the
# container's /src and /sign arguments into Windows paths: leave them alone.
host_path() {
  if command -v cygpath >/dev/null; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

# HOME: the build's user has no home in the image. --user: the build output
# belongs to whoever ran this, not root; rootless Docker already maps the
# container's root to that user, and there --user would map elsewhere.
run=(docker run --rm --init --platform linux/amd64 -e HOME=/tmp
  --mount "type=bind,source=$(host_path "$root"),target=/src" -w /src)
case "$(docker info --format '{{.SecurityOptions}}' 2>/dev/null || true)" in
  *rootless*) ;;
  *) run+=(--user "$(id -u):$(id -g)") ;;
esac
if [ -n "${SMP_WIN_SIGN_PFX:-}" ]; then
  [ -f "$SMP_WIN_SIGN_PFX" ] || die "SMP_WIN_SIGN_PFX: no such file: $SMP_WIN_SIGN_PFX"
  pfx="$(cd "$(dirname "$SMP_WIN_SIGN_PFX")" && pwd)/$(basename "$SMP_WIN_SIGN_PFX")"
  run+=(--mount "type=bind,source=$(host_path "$pfx"),target=/sign/cert.pfx,readonly"
    -e SMP_WIN_SIGN_PFX=/sign/cert.pfx -e SMP_WIN_SIGN_PASS -e SMP_WIN_SIGN_TSA)
fi

echo "==> Building the build image ($image, linux/amd64)"
docker build --platform linux/amd64 -t "$image" "$(host_path "$root/packaging/windows")"

"${run[@]}" "$image" bash /src/packaging/windows/release.sh --in-container

zip="$root/build/release/$name.zip"
[ -f "$zip" ] || die "the container finished without making $zip"
echo "==> Done: $zip"
if command -v shasum >/dev/null; then
  shasum -a 256 "$zip"
else
  sha256sum "$zip"
fi
