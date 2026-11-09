#!/bin/bash
# Build, sign, notarize and verify the release DMG (arm64, macOS 13+).
# Run it on the Mac that holds the signing identity; see DECISIONS.md #29-#34.
#
#   packaging/macos/release.sh [--no-notarize]
#
# SMP_SIGN_ID        signing identity (SHA-1 or name). Default: the single
#                    "Developer ID Application" identity in the keychain.
#                    "-" = ad hoc, only with --no-notarize.
# SMP_NOTARY_PROFILE notarytool keychain profile (default: smalltify), made
#                    once with `xcrun notarytool store-credentials`.
#
# --no-notarize skips notarization, stapling and the Gatekeeper refusal
# check, so the pipeline can be tested before a Developer ID exists.
#
# Output: build/release/Smalltify-Player-<version>-arm64.dmg
set -euo pipefail

die() {
  printf 'release.sh: %s\n' "$*" >&2
  exit 1
}

notarize=1
for arg in "$@"; do
  case "$arg" in
    --no-notarize) notarize=0 ;;
    *) die "unknown argument: $arg (usage: release.sh [--no-notarize])" ;;
  esac
done

root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$root/build-release"
out="$root/build/release"
app_name="Smalltify Player"
profile="${SMP_NOTARY_PROFILE:-smalltify}"

# Resolve the identity before building, so a missing certificate fails fast.
identity="${SMP_SIGN_ID:-}"
if [ -z "$identity" ]; then
  ids="$(security find-identity -v -p codesigning |
    sed -n 's/^ *[0-9]*) \([0-9A-F]*\) "Developer ID Application: .*/\1/p')"
  count="$(printf '%s' "$ids" | grep -c . || true)"
  case "$count" in
    0) die "no 'Developer ID Application' signing identity in the keychain. Install one, or set SMP_SIGN_ID (SMP_SIGN_ID=- with --no-notarize makes an ad hoc test build)." ;;
    1) identity="$ids" ;;
    *)
      printf '%s\n' "$ids" >&2
      die "more than one 'Developer ID Application' identity; set SMP_SIGN_ID to the SHA-1 of the one to use" ;;
  esac
fi
if [ "$identity" = "-" ] && [ "$notarize" = 1 ]; then
  die "SMP_SIGN_ID=- (ad hoc) cannot be notarized; use --no-notarize for a test build"
fi

# --- build ----------------------------------------------------------------
echo "==> Configuring $build (MinSizeRel, arm64, no developer values in .env)"
rm -rf "$build"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSMP_SEED_ENV=OFF
echo "==> Building"
cmake --build "$build" --target smp_bundle

# --- clean bundle ---------------------------------------------------------
# Only what ships: no data/, soloist/ or other build leftovers.
src_app="$build/dist/$app_name.app"
stage="$build/stage/$app_name.app"
rm -rf "$build/stage"
mkdir -p "$stage/Contents/MacOS" "$stage/Contents/Resources"
cp -pX "$src_app/Contents/Info.plist" "$stage/Contents/"
cp -pX "$src_app/Contents/MacOS/smalltify-player" "$stage/Contents/MacOS/"
cp -pX "$src_app/Contents/Resources/AppIcon.icns" \
  "$src_app/Contents/Resources/Inter-LICENSE.txt" \
  "$src_app/Contents/Resources/.env" "$stage/Contents/Resources/"
xattr -cr "$stage"

cmp -s "$stage/Contents/Resources/.env" "$root/.env.example" ||
  die ".env in the bundle is not byte-identical to .env.example"

exe="$stage/Contents/MacOS/smalltify-player"
archs="$(lipo -archs "$exe")"
[ "$archs" = "arm64" ] || die "binary architectures are '$archs', expected arm64"
minos="$(otool -l "$exe" | awk '/LC_BUILD_VERSION/ {f=1} f && $1=="minos" {print $2; f=0}')"
target="$(plutil -extract LSMinimumSystemVersion raw -o - "$stage/Contents/Info.plist")"
[ "$minos" = "$target" ] || die "binary minos is $minos, but LSMinimumSystemVersion is $target"

version="$(plutil -extract CFBundleShortVersionString raw -o - "$stage/Contents/Info.plist")"

# --- sign -----------------------------------------------------------------
sign() {
  codesign --force --timestamp --sign "$identity" "$@"
}

echo "==> Signing the app ($identity)"
sign "$stage/Contents/Frameworks/libvulkan.1.dylib"
sign "$stage/Contents/Frameworks/libMoltenVK.dylib"
sign --options runtime --entitlements "$root/packaging/macos/entitlements.plist" "$stage"
codesign --verify --strict --deep -vv "$stage"

# --- disk image -----------------------------------------------------------
dmg_root="$build/dmg"
dmg="$out/Smalltify-Player-$version-arm64.dmg"
rm -rf "$dmg_root"
mkdir -p "$dmg_root" "$out"
cp -RpX "$stage" "$dmg_root/"
ln -s /Applications "$dmg_root/Applications"
rm -f "$dmg"
echo "==> Creating $dmg"
hdiutil create -volname "Smalltify Player $version" -srcfolder "$dmg_root" \
  -ov -format UDZO "$dmg"
echo "==> Signing the disk image"
sign "$dmg"

# --- notarize -------------------------------------------------------------
if [ "$notarize" = 1 ]; then
  echo "==> Notarizing (profile: $profile); waiting for Apple"
  json="$build/notary-submit.json"
  set +e
  xcrun notarytool submit "$dmg" --wait --keychain-profile "$profile" \
    --output-format json > "$json"
  rc=$?
  set -e
  status="$(plutil -extract status raw -o - "$json" 2>/dev/null || true)"
  id="$(plutil -extract id raw -o - "$json" 2>/dev/null || true)"
  if [ "$status" != "Accepted" ]; then
    if [ -n "$id" ]; then
      echo "==> Notarization status: $status; log:" >&2
      xcrun notarytool log "$id" --keychain-profile "$profile" >&2 || true
    else
      cat "$json" >&2
    fi
    die "notarization failed (status: ${status:-unknown}, exit $rc)"
  fi
  echo "==> Stapling"
  xcrun stapler staple "$dmg"
  xcrun stapler validate "$dmg"
else
  echo "==> Skipping notarization (--no-notarize)"
fi

# --- verify the copy users get: the app inside the final disk image -------
mnt="$build/mnt"
rm -rf "$mnt"
mkdir -p "$mnt"
hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$dmg" >/dev/null
trap 'hdiutil detach -quiet "$mnt" >/dev/null 2>&1 || true' EXIT
mounted="$mnt/$app_name.app"

codesign --verify --strict --deep -vv "$mounted"

# spctl prints its verdict on stderr. Under --no-notarize a refusal is expected.
gatekeeper() { # gatekeeper <what> <spctl args...>
  local what="$1"
  shift
  if "$@"; then
    return 0
  fi
  [ "$notarize" = 1 ] && die "Gatekeeper rejected $what"
  echo "(Gatekeeper rejects $what, as expected for --no-notarize)"
}
echo "==> Gatekeeper"
gatekeeper "the app" spctl -a -t exec -vv "$mounted"
gatekeeper "the disk image" spctl -a -t open --context context:primary-signature -vv "$dmg"

trap - EXIT
hdiutil detach -quiet "$mnt"

echo "==> Done: $dmg"
shasum -a 256 "$dmg"
