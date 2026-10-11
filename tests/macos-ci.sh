#!/bin/sh
# Build the same native bundle locally and on a disposable macOS runner.
set -eu

test "$#" -ge 2 && test "$#" -le 3 || {
    printf 'Usage: sh tests/macos-ci.sh QT_PREFIX|auto BUILD_DIRECTORY [prepare|build|test|package|all]\n' >&2; exit 2;
}
phase=${3:-all}
export SQUADSPEAK_VERSION=${SQUADSPEAK_VERSION:-${VERSION:-}}
export SQUADSPEAK_UPDATE_PUBLIC_KEY=${SQUADSPEAK_UPDATE_PUBLIC_KEY:-${UPDATE_PUBLIC_KEY:-}}
case "$phase" in prepare|build|test|package|all) ;; *) printf 'Unknown phase: %s\n' "$phase" >&2; exit 2 ;; esac
qt_prefix=$1
mkdir -p "$2"
build_root=$(cd "$2" && pwd)
source_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
if test "$qt_prefix" = auto; then qt_prefix="$build_root/toolchain/Qt/6.11.3/macos"; fi
if test "$phase" = prepare; then
    brew install cmake ninja meson pkgconf nasm ccache
    if test ! -x "$qt_prefix/bin/qmake"; then
        python3 -m venv "$build_root/toolchain/aqt"
        "$build_root/toolchain/aqt/bin/pip" install --disable-pip-version-check \
            'git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b'
        "$build_root/toolchain/aqt/bin/aqt" install-qt mac desktop 6.11.3 clang_64 \
            --outputdir "$build_root/toolchain/Qt" --modules qtmultimedia qtimageformats
    fi
    test "$("$qt_prefix/bin/qmake" -query QT_VERSION)" = 6.11.3
    exit 0
fi
qt_prefix=$(cd "$qt_prefix" && pwd)
architecture=$(uname -m)
case "$architecture" in
    arm64) openssl_target=darwin64-arm64-cc ;;
    x86_64) openssl_target=darwin64-x86_64-cc ;;
    *) printf 'Unsupported macOS architecture: %s\n' "$architecture" >&2; exit 2 ;;
esac
export MACOSX_DEPLOYMENT_TARGET=13.0
mkdir -p "$build_root/dependencies" "$build_root/artifacts"

fetch() {
    name=$1 url=$2 digest=$3
    archive="$build_root/dependencies/$name"
    if test ! -f "$archive"; then
        curl --fail --location --connect-timeout 20 --max-time 180 --retry 3 --output "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 --check
    directory=${name%.tar.*}
    if test ! -d "$build_root/dependencies/$directory"; then
        tar -xf "$archive" -C "$build_root/dependencies"
    fi
}
prefix="$build_root/dependencies/prefix"
if test "$phase" = build || test "$phase" = all; then
stamp="$prefix/.build-inputs"
inputs=$( { shasum -a 256 "$0"; xcrun clang --version; xcrun --show-sdk-version; printf '%s\n' "$architecture" "$MACOSX_DEPLOYMENT_TARGET"; } | shasum -a 256 | cut -d ' ' -f 1)
if test -f "$stamp"; then
    test "$(cat "$stamp")" = "$inputs" || { printf 'Dependency cache inputs do not match.\n' >&2; exit 1; }
    for file in include/openssl/ssl.h include/opus/opus.h include/samplerate.h \
        lib/libssl.dylib lib/libcrypto.dylib lib/libopus.dylib lib/libsamplerate.dylib; do
        test -s "$prefix/$file" || { printf 'Incomplete dependency cache: %s\n' "$file" >&2; exit 1; }
    done
    printf 'Reusing validated native dependencies.\n'
else
fetch openssl-3.6.5.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
fetch opus-1.6.1.tar.gz https://distfiles.macports.org/libopus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
fetch libsamplerate-0.2.2.tar.xz https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz 3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893
fetch ffmpeg-7.1.5.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

(
    cd "$build_root/dependencies/openssl-3.6.5"
    ./Configure "$openssl_target" shared no-tests --prefix="$prefix" --libdir=lib \
        "-O3" "-mmacosx-version-min=13.0"
    make -j 3
    make install_sw
)
for dependency in opus-1.6.1 libsamplerate-0.2.2; do
    cmake -S "$build_root/dependencies/$dependency" -B "$build_root/dependencies/$dependency-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$architecture" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DLIBSAMPLERATE_EXAMPLES=OFF -DOPUS_BUILD_PROGRAMS=OFF
    cmake --build "$build_root/dependencies/$dependency-build" --parallel 3
    cmake --install "$build_root/dependencies/$dependency-build"
done

printf '%s\n' "$inputs" > "$stamp"
fi
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
set --
if test -n "${SQUADSPEAK_VERSION:-}"; then set -- "-DSQUADSPEAK_VERSION=$SQUADSPEAK_VERSION"; fi
if command -v ccache >/dev/null 2>&1; then
    set -- "$@" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
fi
if test -n "${SQUADSPEAK_UPDATE_PUBLIC_KEY:-}"; then
    set -- "$@" "-DSQUADSPEAK_UPDATE_PUBLIC_KEY=$SQUADSPEAK_UPDATE_PUBLIC_KEY"
fi
cmake -S "$source_root" -B "$build_root/app" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH="$qt_prefix;$prefix" \
    -DCMAKE_OSX_ARCHITECTURES="$architecture" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DOPENSSL_ROOT_DIR="$prefix" \
    -DFETCHCONTENT_SOURCE_DIR_FFMPEG="$build_root/dependencies/ffmpeg-7.1.5" "$@"
cmake --build "$build_root/app" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
if command -v ccache >/dev/null 2>&1; then ccache --show-stats; fi
fi
if test "$phase" = test || test "$phase" = all; then
result=0
network_tests='^(channel_contract.*|channel_controls)$'
set --
if test "${GITHUB_ACTIONS:-}" = true; then set -- -E "$network_tests"; fi
QT_LOGGING_RULES='squadspeak.discovery.debug=true' ctest --parallel "${CTEST_PARALLEL_LEVEL:-4}" --test-dir "$build_root/app" --output-on-failure --no-tests=error \
    --output-junit "$build_root/artifacts/ctest.xml" --output-log "$build_root/artifacts/ctest.log" "$@" || result=$?
cp "$build_root/app/Testing/Temporary/LastTest.log" "$build_root/artifacts/test-details.log"
if test "${GITHUB_ACTIONS:-}" = true; then
    # Hosted macOS runners lack interactive LAN consent (runner-images#10924).
    # Only disposable-runner network contracts use Apple's root exemption.
    # Measure realtime playout at the application's normal logging level.
    sudo -n env QT_LOGGING_RULES='' \
        "$(command -v ctest)" --parallel "${CTEST_PARALLEL_LEVEL:-4}" --test-dir "$build_root/app" --output-on-failure --no-tests=error \
        -R "$network_tests" --output-junit "$build_root/artifacts/network.xml" \
        --output-log "$build_root/artifacts/network.log" || result=$?
    cat "$build_root/artifacts/network.log" >> "$build_root/artifacts/ctest.log"
    cat "$build_root/app/Testing/Temporary/LastTest.log" >> "$build_root/artifacts/test-details.log"
fi
if test -d "$build_root/app/smoke"; then cp -R "$build_root/app/smoke" "$build_root/artifacts/"; fi
# Keep native memory checks in the same test phase.
# Exclude only two Apple-owned startup allocations. Codec and application
# allocations remain checked, including VideoToolbox's dropped-frame path.
/usr/bin/leaks '-exclude=-[LNProcessInstanceRegistryClient makeXPCConnection]' \
    -exclude=AVControlCenterAudioPreferredMicrophoneModePreferenceKey -atExit -- \
    "$build_root/app/video_tests" roundTripAndQualityChanges \
    > "$build_root/artifacts/video-leaks.log" 2>&1 || result=$?
cat "$build_root/artifacts/video-leaks.log"
# leaks reports its own findings, not the wrapped test's exit status.
if ! grep -Eq '^Totals: [1-9][0-9]* passed, 0 failed, 0 skipped, 0 blacklisted,' "$build_root/artifacts/video-leaks.log"; then
    printf 'The video contract did not pass under native leak instrumentation.\n' >&2
    result=1
fi
if test "$architecture" = x86_64; then
    # Guard pages also catch writes in uninstrumented codec assembly, which
    # AddressSanitizer cannot observe in a prebuilt dependency.
    QT_QPA_PLATFORM=offscreen DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
        "$build_root/app/video_tests" capturedFrameKeepsQtSurfaceConversion \
        decodedImagesKeepIndependentStorage > "$build_root/artifacts/video-guard.log" 2>&1 || result=$?
    cat "$build_root/artifacts/video-guard.log"
fi
if grep -q '^SKIP *:' "$build_root/artifacts/test-details.log"; then
    printf 'A contract was skipped; inspect test-details.log.\n' >&2
    exit 1
fi
test "$result" -eq 0
fi
if test "$phase" = package || test "$phase" = all; then
# File providers can re-add Finder metadata between cleanup and codesign.
# Stage signed bundles outside synced workspaces; keep only archives/reports.
stage_root=$(mktemp -d "${TMPDIR:-/tmp}/squadspeak-package.XXXXXX")
trap 'rm -rf "$stage_root"' EXIT
if ! cmake --install "$build_root/app" --prefix "$stage_root/install"; then
    # Preserve layout evidence before removing a failed deployment stage.
    for binary in "$build_root/app/squadspeak.app/Contents/MacOS/squadspeak" \
        "$build_root/app/squad_image_worker" \
        "$stage_root/install/squadspeak.app/Contents/MacOS/squadspeak" \
        "$stage_root/install/squadspeak.app/Contents/MacOS/squad_image_worker"; do
        if test -f "$binary"; then
            printf '\n%s\n' "$binary" >> "$build_root/artifacts/deployment-layout.log"
            otool -l "$binary" >> "$build_root/artifacts/deployment-layout.log" 2>&1 || true
        fi
    done
    exit 1
fi
app="$stage_root/install/squadspeak.app"
if test "${MACOS_SIGNING_IDENTITY:--}" != -; then
    cat > "$app/Contents/Resources/entitlements.plist" <<'ENTITLEMENTS'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>
ENTITLEMENTS
    "$qt_prefix/bin/macdeployqt" "$app" "-sign-for-notarization=$MACOS_SIGNING_IDENTITY"
    codesign --verify --deep --strict "$app"
fi
if test -n "${MACOS_NOTARY_PROFILE:-}"; then
    ditto -c -k --sequesterRsrc --keepParent "$app" "$stage_root/notarization.zip"
    xcrun notarytool submit "$stage_root/notarization.zip" --keychain-profile "$MACOS_NOTARY_PROFILE" \
        --keychain "$RUNNER_TEMP/native-build.keychain-db" --wait --timeout 15m
    xcrun stapler staple "$app"
    xcrun stapler validate "$app"
fi
python3 "$source_root/tests/check_macos_bundle.py" "$stage_root/install/squadspeak.app" \
    --minimum-system 13.0 --architecture "$architecture" --report "$build_root/artifacts/bundle.json" \
    --qml-reference "$build_root/app/qt-qml/lib/QtQml.framework/Versions/A/QtQml"
ditto -c -k --sequesterRsrc --keepParent "$stage_root/install/squadspeak.app" \
    "$build_root/artifacts/squadspeak-macos-$architecture.zip"
mkdir -p "$stage_root/unpacked"
ditto -x -k "$build_root/artifacts/squadspeak-macos-$architecture.zip" "$stage_root/unpacked"
python3 "$source_root/tests/check_macos_bundle.py" "$stage_root/unpacked/squadspeak.app" \
    --minimum-system 13.0 --architecture "$architecture" --report "$build_root/artifacts/archive.json" \
    --qml-reference "$build_root/app/qt-qml/lib/QtQml.framework/Versions/A/QtQml"
if test "${SIGN_UPDATES:-false}" = true; then
    test -n "${UPDATE_PRIVATE_KEY:-}" && test -n "${SQUADSPEAK_UPDATE_PUBLIC_KEY:-}"
    key="$stage_root/update-key.b64"
    (umask 077; printf '%s' "$UPDATE_PRIVATE_KEY" > "$key")
    python3 - "$key" "$stage_root/update-key.der" "$SQUADSPEAK_UPDATE_PUBLIC_KEY" "$prefix/bin/openssl" <<'PYKEY'
import base64, pathlib, subprocess, sys
seed = base64.b64decode(pathlib.Path(sys.argv[1]).read_text().strip(), validate=True)
if len(seed) != 32:
    raise SystemExit("Update signing key must contain 32 bytes")
der = pathlib.Path(sys.argv[2])
der.write_bytes(bytes.fromhex("302e020100300506032b657004220420") + seed)
pub = subprocess.check_output([sys.argv[4], "pkey", "-inform", "DER", "-in", str(der), "-pubout", "-outform", "DER"])
if base64.b64encode(pub[-32:]).decode() != sys.argv[3]:
    raise SystemExit("Update private/public key mismatch")
PYKEY
    signer="$build_root/app/_deps/sparkle-src/bin/sign_update"
    archive="$build_root/artifacts/squadspeak-macos-$architecture.zip"
    signature=$("$signer" --ed-key-file "$key" -p "$archive")
    "$signer" --ed-key-file "$key" --verify "$archive" "$signature"
    case "$architecture" in arm64) flag=arm ;; x86_64) flag=x86 ;; esac
    python3 "$source_root/cmake/build_appcasts.py" --version "$SQUADSPEAK_VERSION" \
        --repository YunaBraska/SquadSpeak --output-dir "$build_root/artifacts" \
        --public-key "$SQUADSPEAK_UPDATE_PUBLIC_KEY" "--$flag-archive" "$archive" "--$flag-signature" "$signature"
    feed="$build_root/artifacts/appcast-macos-$architecture.xml"
    "$signer" --ed-key-file "$key" "$feed"
    "$signer" --ed-key-file "$key" --verify "$feed"
fi
fi
