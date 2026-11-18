#!/bin/sh
set -eu

test "$#" -eq 2 || { printf '%s\n' 'Expected marketing version and build number.' >&2; exit 2; }
: "${APP_STORE_BUNDLE_ID:?}" "${APP_STORE_TEAM_ID:?}"
: "${IOS_APP_STORE_DISTRIBUTION_IDENTITY:?}" "${IOS_APP_STORE_PROFILE_NAME:?}"
: "${IOS_APP_STORE_PROFILE_PATH:?}" "${IOS_APP_STORE_PROFILE_UUID:?}"

case "$1" in
    *[!0-9.]*|*..*|.*|*.) printf '%s\n' 'Invalid marketing version.' >&2; exit 2 ;;
esac
case "$2" in *[!0-9]*|'') printf '%s\n' 'Invalid build number.' >&2; exit 2 ;; esac

root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d "${RUNNER_TEMP:-/tmp}/squadspeak-ios-store.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
toolchain="$stage/toolchain"
qt_root="$toolchain/Qt"
ios_qt="$qt_root/6.11.3/ios"
host_qt="$qt_root/6.11.3/macos"
deps="$stage/dependencies/ios-device"

command -v brew >/dev/null || { printf '%s\n' 'Homebrew is required on the macOS runner.' >&2; exit 2; }
brew install cmake ninja meson pkgconf nasm protobuf
for command in cmake ninja meson pkg-config nasm protoc python3 rustup xcrun; do
    command -v "$command" >/dev/null || { printf '%s is required.\n' "$command" >&2; exit 2; }
done
test -f "$IOS_APP_STORE_PROFILE_PATH"
test -x "$(command -v xcodebuild)"

if test ! -x "$ios_qt/bin/qt-cmake"; then
    python3 -m venv "$toolchain/aqt"
    "$toolchain/aqt/bin/pip" install --disable-pip-version-check \
        'git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b'
    "$toolchain/aqt/bin/aqt" install-qt mac ios 6.11.3 ios --autodesktop \
        --base https://master.qt.io --outputdir "$qt_root" -m qtmultimedia qtimageformats
fi
test "$("$ios_qt/bin/qmake" -query QT_VERSION)" = 6.11.3
test -x "$host_qt/bin/qmake"

rustup target add aarch64-apple-ios
sh "$root/cmake/build-mobile-dependencies.sh" ios-device "$deps" "$stage/dependency-build" 17.0

unset SQUADSPEAK_UPDATE_PUBLIC_KEY UPDATE_PUBLIC_KEY
"$ios_qt/bin/qt-cmake" -S "$root" -B "$stage/build" -G Xcode \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_PREFIX_PATH="$ios_qt" -DQT_HOST_PATH="$host_qt" \
    -DSQUADSPEAK_MOBILE_DEPS_ROOT="$deps" -DSQUADSPEAK_STORE_BUILD=ON \
    -DSQUADSPEAK_BUNDLE_ID="$APP_STORE_BUNDLE_ID" \
    -DSQUADSPEAK_VERSION="$1" -DSQUADSPEAK_APPLE_BUILD_NUMBER="$2" \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Manual \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=YES \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="$APP_STORE_TEAM_ID" \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="$IOS_APP_STORE_DISTRIBUTION_IDENTITY" \
    -DCMAKE_XCODE_ATTRIBUTE_PROVISIONING_PROFILE="$IOS_APP_STORE_PROFILE_UUID" \
    -DCMAKE_XCODE_ATTRIBUTE_PROVISIONING_PROFILE_SPECIFIER="$IOS_APP_STORE_PROFILE_NAME"
cmake --build "$stage/build" --config Release --target squadspeak --parallel 3

app_count=$(find "$stage/build" -type d -name squadspeak.app -print | wc -l | tr -d ' ')
test "$app_count" = 1 || { printf '%s\n' 'Expected exactly one signed iPhone and iPad app bundle.' >&2; exit 1; }
app=$(find "$stage/build" -type d -name squadspeak.app -print -quit)
plist="$app/Info.plist"
test -f "$plist"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")" = "$APP_STORE_BUNDLE_ID"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")" = "$1"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")" = "$2"
test -f "$app/embedded.mobileprovision"
codesign --verify --deep --strict --verbose=2 "$app"

package_root="$stage/package"
mkdir -p "$package_root/Payload" "$root/dist/app-store"
ditto "$app" "$package_root/Payload/SquadSpeak.app"
test ! -e "$root/dist/app-store/SquadSpeak.ipa" || {
    printf '%s\n' 'Store output already exists. Remove or move it before packaging.' >&2; exit 1;
}
(cd "$package_root" && ditto -c -k --sequesterRsrc --keepParent Payload "$root/dist/app-store/SquadSpeak.ipa")
