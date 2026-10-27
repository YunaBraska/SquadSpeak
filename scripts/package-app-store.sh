#!/bin/sh
set -eu

test "$#" -eq 2 || { printf 'Expected marketing version and build number.\n' >&2; exit 2; }
: "${APP_STORE_BUNDLE_ID:?}" "${APP_STORE_TEAM_ID:?}"
: "${MACOS_APP_STORE_APPLICATION_IDENTITY:?}" "${MACOS_APP_STORE_INSTALLER_IDENTITY:?}"
: "${MACOS_APP_STORE_PROFILE_PATH:?}"
case "$2" in *[!0-9]*|'') printf 'Invalid build number.\n' >&2; exit 2 ;; esac
root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d "${RUNNER_TEMP:-/tmp}/squadspeak-store.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
export SQUADSPEAK_VERSION="$1" SQUADSPEAK_STORE_BUILD=ON
unset SQUADSPEAK_UPDATE_PUBLIC_KEY UPDATE_PUBLIC_KEY
sh "$root/tests/macos-ci.sh" auto "$stage/build" prepare
sh "$root/tests/macos-ci.sh" auto "$stage/build" build
cmake --install "$stage/build/app" --prefix "$stage/install"
app="$stage/install/squadspeak.app"
test ! -d "$app/Contents/Frameworks/Sparkle.framework"
plist="$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $APP_STORE_BUNDLE_ID" "$plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $1" "$plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $2" "$plist"
cp "$MACOS_APP_STORE_PROFILE_PATH" "$app/Contents/embedded.provisionprofile"
cp "$root/scripts/app-store.entitlements" "$stage/app.entitlements"
/usr/libexec/PlistBuddy -c "Add :com.apple.application-identifier string $APP_STORE_TEAM_ID.$APP_STORE_BUNDLE_ID" "$stage/app.entitlements"
/usr/libexec/PlistBuddy -c "Add :com.apple.developer.team-identifier string $APP_STORE_TEAM_ID" "$stage/app.entitlements"

# Sign nested Mach-O code before its enclosing frameworks and application.
find "$app/Contents" -type f ! -path '*/MacOS/squadspeak' ! -path '*/MacOS/squad_image_worker' -print | while IFS= read -r item; do
    if file -b "$item" | grep -q 'Mach-O'; then
        codesign --force --sign "$MACOS_APP_STORE_APPLICATION_IDENTITY" --timestamp "$item"
    fi
done
find "$app/Contents/Frameworks" -depth -type d -name '*.framework' -print | while IFS= read -r framework; do
    codesign --force --sign "$MACOS_APP_STORE_APPLICATION_IDENTITY" --timestamp "$framework"
done
codesign --force --sign "$MACOS_APP_STORE_APPLICATION_IDENTITY" --timestamp \
    --entitlements "$root/scripts/app-store-helper.entitlements" "$app/Contents/MacOS/squad_image_worker"
codesign --force --sign "$MACOS_APP_STORE_APPLICATION_IDENTITY" --timestamp \
    --entitlements "$stage/app.entitlements" "$app"
codesign --verify --deep --strict "$app"
mkdir -p "$root/dist/app-store"
test ! -e "$root/dist/app-store/SquadSpeak.app" && test ! -e "$root/dist/app-store/SquadSpeak.pkg" || {
    printf 'Store outputs already exist. Remove or move them before packaging.\n' >&2; exit 1;
}
ditto "$app" "$root/dist/app-store/SquadSpeak.app"
productbuild --component "$root/dist/app-store/SquadSpeak.app" /Applications \
    --sign "$MACOS_APP_STORE_INSTALLER_IDENTITY" "$root/dist/app-store/SquadSpeak.pkg"
