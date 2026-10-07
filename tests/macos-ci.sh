#!/bin/sh
# Build the same native bundle locally and on a disposable macOS runner.
set -eu

test "$#" -eq 2 || { printf 'Usage: sh tests/macos-ci.sh QT_PREFIX BUILD_DIRECTORY\n' >&2; exit 2; }
qt_prefix=$(cd "$1" && pwd)
mkdir -p "$2"
build_root=$(cd "$2" && pwd)
source_root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
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
        curl --fail --location --retry 3 --output "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 --check
    tar -xf "$archive" -C "$build_root/dependencies"
}
fetch openssl-3.6.5.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
fetch opus-1.6.1.tar.gz https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
fetch libsamplerate-0.2.2.tar.xz https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz 3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893
fetch ffmpeg-7.1.5.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

prefix="$build_root/dependencies/prefix"
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

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
set --
if test -n "${SQUADSPEAK_VERSION:-}"; then set -- "-DSQUADSPEAK_VERSION=$SQUADSPEAK_VERSION"; fi
if test -n "${SQUADSPEAK_UPDATE_PUBLIC_KEY:-}"; then
    set -- "$@" "-DSQUADSPEAK_UPDATE_PUBLIC_KEY=$SQUADSPEAK_UPDATE_PUBLIC_KEY"
fi
cmake -S "$source_root" -B "$build_root/app" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH="$qt_prefix;$prefix" \
    -DCMAKE_OSX_ARCHITECTURES="$architecture" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DOPENSSL_ROOT_DIR="$prefix" \
    -DFETCHCONTENT_SOURCE_DIR_FFMPEG="$build_root/dependencies/ffmpeg-7.1.5" "$@"
cmake --build "$build_root/app" --parallel 3
result=0
network_tests='^(channel_contract|channel_controls)$'
set --
if test "${GITHUB_ACTIONS:-}" = true; then set -- -E "$network_tests"; fi
QT_LOGGING_RULES='squadspeak.discovery.debug=true' ctest --test-dir "$build_root/app" --output-on-failure --no-tests=error \
    --output-junit "$build_root/artifacts/ctest.xml" "$@" > "$build_root/artifacts/ctest.log" 2>&1 || result=$?
cp "$build_root/app/Testing/Temporary/LastTest.log" "$build_root/artifacts/test-details.log"
if test "${GITHUB_ACTIONS:-}" = true; then
    # Hosted macOS runners lack interactive LAN consent (runner-images#10924).
    # Only disposable-runner network contracts use Apple's root exemption.
    # Measure realtime playout at the application's normal logging level.
    sudo -n env QT_LOGGING_RULES='' \
        "$(command -v ctest)" --test-dir "$build_root/app" --output-on-failure --no-tests=error \
        -R "$network_tests" --output-junit "$build_root/artifacts/network.xml" \
        > "$build_root/artifacts/network.log" 2>&1 || result=$?
    cat "$build_root/artifacts/network.log" >> "$build_root/artifacts/ctest.log"
    cat "$build_root/app/Testing/Temporary/LastTest.log" >> "$build_root/artifacts/test-details.log"
fi
if test -d "$build_root/app/smoke"; then cp -R "$build_root/app/smoke" "$build_root/artifacts/"; fi
cat "$build_root/artifacts/ctest.log"
# Collect independent memory/package evidence even when a contract failed.
# The final exit status still prevents publishing any failing build.
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
python3 "$source_root/tests/check_macos_bundle.py" "$stage_root/install/squadspeak.app" \
    --minimum-system 13.0 --architecture "$architecture" --report "$build_root/artifacts/bundle.json"
ditto -c -k --sequesterRsrc --keepParent "$stage_root/install/squadspeak.app" \
    "$build_root/artifacts/squadspeak-macos-$architecture.zip"
mkdir -p "$stage_root/unpacked"
ditto -x -k "$build_root/artifacts/squadspeak-macos-$architecture.zip" "$stage_root/unpacked"
python3 "$source_root/tests/check_macos_bundle.py" "$stage_root/unpacked/squadspeak.app" \
    --minimum-system 13.0 --architecture "$architecture" --report "$build_root/artifacts/archive.json"
test "$result" -eq 0
