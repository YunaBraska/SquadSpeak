#!/bin/sh
set -eu

if test "$#" -lt 1 || test "$#" -gt 3; then
    printf '%s\n' 'Usage: sh cmake/build-ios-dependencies.sh PREFIX [BUILD_ROOT] [DEPLOYMENT_TARGET]' >&2
    exit 2
fi

prefix=$(CDPATH='' cd "$1" 2>/dev/null && pwd || { mkdir -p "$1"; CDPATH='' cd "$1"; pwd; })
build_root=${2:-"$prefix/.build"}
deployment_target=${3:-17.0}
config_revision=3
sdk=${IOS_SDK:-iphonesimulator}
architecture=${IOS_ARCH:-$(uname -m)}
test "$sdk" = iphonesimulator || { printf '%s\n' 'This script currently supports only the iOS Simulator SDK.' >&2; exit 2; }
case "$architecture" in
    arm64|x86_64) ;;
    *) printf '%s\n' 'IOS_ARCH must be arm64 or x86_64.' >&2; exit 2 ;;
esac

for tool in cmake ninja xcrun make curl shasum tar lipo ar; do
    command -v "$tool" >/dev/null || { printf '%s\n' "$tool is required." >&2; exit 2; }
done

mkdir -p "$build_root/sources"
sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
clang=$(xcrun --sdk "$sdk" --find clang)
clangxx=$(xcrun --sdk "$sdk" --find clang++)
target="${architecture}-apple-ios${deployment_target}-simulator"
openssl_target="iossimulator-${architecture}-xcrun"
identity_file="$prefix/.squadspeak-ios-dependencies"
identity=$(cat <<EOF
sdk=$sdk
architecture=$architecture
deployment_target=$deployment_target
config_revision=$config_revision
sysroot=$sysroot
EOF
)

if test -f "$identity_file"; then
    test "$(cat "$identity_file")" = "$identity" || {
        printf '%s\n' "Dependency prefix identity mismatch: $prefix" >&2
        exit 2
    }
else
    for output in \
        "$prefix/include/openssl/ssl.h" \
        "$prefix/lib/libcrypto.a" \
        "$prefix/lib/libssl.a" \
        "$prefix/include/opus/opus.h" \
        "$prefix/lib/libopus.a" \
        "$prefix/include/samplerate.h" \
        "$prefix/lib/libsamplerate.a" \
        "$prefix/include/libavcodec/avcodec.h" \
        "$prefix/include/libavutil/avutil.h" \
        "$prefix/include/libswscale/swscale.h" \
        "$prefix/include/libavformat/avformat.h" \
        "$prefix/include/libswresample/swresample.h" \
        "$prefix/lib/libavcodec.a" \
        "$prefix/lib/libavutil.a" \
        "$prefix/lib/libswscale.a" \
        "$prefix/lib/libavformat.a" \
        "$prefix/lib/libswresample.a"; do
        if test -e "$output"; then
            printf '%s\n' "Unmarked dependency prefix contains output: $prefix" >&2
            printf '%s\n' 'Use a fresh prefix after an interrupted or architecture-switched build.' >&2
            exit 2
        fi
    done
fi

validate_static_library() {
    library=$1
    test -f "$library" || return 1
    lipo -verify_arch "$architecture" "$library" >/dev/null 2>&1 || return 1
    check_dir=$(mktemp -d "${TMPDIR:-/tmp}/squad-ios-library.XXXXXX")
    member=$(ar -t "$library" | awk '$0 !~ /^__\.SYMDEF/ { print; exit }')
    test -n "$member" || { rmdir "$check_dir"; return 1; }
    if ! (cd "$check_dir" && ar -x "$library" "$member" &&
        xcrun vtool -show-build "$check_dir/$member" 2>/dev/null |
        grep -q 'platform IOSSIMULATOR'); then
        rm -rf "$check_dir"
        return 1
    fi
    rm -rf "$check_dir"
    return 0
}

validate_prefix_architecture() {
    for library in \
        "$prefix/lib/libcrypto.a" \
        "$prefix/lib/libssl.a" \
        "$prefix/lib/libopus.a" \
        "$prefix/lib/libsamplerate.a" \
        "$prefix/lib/libavcodec.a" \
        "$prefix/lib/libavutil.a" \
        "$prefix/lib/libswscale.a" \
        "$prefix/lib/libavformat.a" \
        "$prefix/lib/libswresample.a"; do
        validate_static_library "$library" || {
            printf 'Invalid iOS Simulator archive or architecture: %s\n' "$library" >&2
            return 1
        }
    done
}

fetch() {
    name=$1
    url=$2
    digest=$3
    archive="$build_root/sources/$name"
    if test ! -f "$archive"; then
        curl --fail --location --connect-timeout 20 --max-time 180 --retry 3 --output "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 --check
    directory="$build_root/sources/${name%.tar.gz}"
    directory="${directory%.tar.xz}"
    if test ! -d "$directory"; then
        tar -xf "$archive" -C "$build_root/sources"
    fi
}

fetch openssl-3.6.5.tar.gz \
    https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz \
    a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
fetch opus-1.6.1.tar.gz \
    https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz \
    6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
fetch libsamplerate-0.2.2.tar.xz \
    https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz \
    3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893
fetch ffmpeg-7.1.5.tar.xz \
    https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz \
    de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

openssl_source="$build_root/sources/openssl-3.6.5"
if test ! -f "$prefix/include/openssl/ssl.h" || \
   test ! -f "$prefix/lib/libcrypto.a" || \
   test ! -f "$prefix/lib/libssl.a"; then
    (
        cd "$openssl_source"
        CFLAGS="-O3 -isysroot $sysroot -mios-simulator-version-min=$deployment_target" \
        LDFLAGS="-isysroot $sysroot -mios-simulator-version-min=$deployment_target" \
            ./Configure "$openssl_target" no-shared no-tests --prefix="$prefix" --libdir=lib
        make -j3
        make install_sw
    )
fi

toolchain="$build_root/ios-toolchain.cmake"
cat > "$toolchain" <<EOF
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_SYSROOT $sdk)
set(CMAKE_OSX_ARCHITECTURES $architecture)
set(CMAKE_OSX_DEPLOYMENT_TARGET $deployment_target)
set(CMAKE_C_COMPILER $clang)
set(CMAKE_CXX_COMPILER $clangxx)
EOF

for dependency in opus-1.6.1 libsamplerate-0.2.2; do
    if test "$dependency" = opus-1.6.1; then
        output="$prefix/lib/libopus.a"
        header="$prefix/include/opus/opus.h"
    else
        output="$prefix/lib/libsamplerate.a"
        header="$prefix/include/samplerate.h"
    fi
    if test ! -f "$output" || test ! -f "$header" || ! validate_static_library "$output"; then
        build="$build_root/$dependency-$architecture-build"
        cmake -S "$build_root/sources/$dependency" -B "$build" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_OSX_ARCHITECTURES="$architecture" \
            -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
            -DCMAKE_OSX_SYSROOT="$sdk" \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF \
            -DBUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DLIBSAMPLERATE_EXAMPLES=OFF
        cmake --build "$build" --parallel 2
        cmake --install "$build"
    fi
done

ffmpeg_source="$build_root/sources/ffmpeg-7.1.5"
if test ! -f "$prefix/lib/libavcodec.a" || \
   test ! -f "$prefix/lib/libavutil.a" || \
   test ! -f "$prefix/lib/libswscale.a" || \
   test ! -f "$prefix/lib/libavformat.a" || \
   test ! -f "$prefix/lib/libswresample.a" || \
   ! validate_prefix_architecture 2>/dev/null; then
    ffmpeg_cpu_flags=""
    if test "$architecture" = x86_64; then
        # The iOS Simulator translates x86_64 code on Apple Silicon. FFmpeg's
        # inline x86 swscale path produces corrupted MPEG-4 colors there.
        # Keep the x86_64 simulator scalar for deterministic media correctness.
        # The arm64 simulator build retains its native optimized path.
        ffmpeg_cpu_flags="--disable-x86asm --disable-runtime-cpudetect --disable-mmx --disable-mmxext --disable-sse --disable-sse2 --disable-sse3 --disable-sse4 --disable-sse42"
    fi
    (
        cd "$ffmpeg_source"
        ./configure --prefix="$prefix" --target-os=darwin --arch="$architecture" --enable-cross-compile \
            --cc="$clang" --sysroot="$sysroot" --disable-programs --disable-doc --disable-debug \
            --disable-autodetect --disable-shared --enable-static --enable-network --enable-avformat \
            --enable-swresample --disable-avfilter --disable-avdevice --enable-videotoolbox \
            --enable-securetransport --enable-pic \
            $ffmpeg_cpu_flags \
            --extra-cflags="-target $target -isysroot $sysroot" \
            --extra-ldflags="-target $target -isysroot $sysroot"
        make -j3
        make install
    )
fi

validate_prefix_architecture || exit 1
printf '%s\n' "$identity" > "$identity_file"
printf 'iOS Simulator dependency prefix ready for %s: %s\n' "$architecture" "$prefix"
printf 'APM is built by the main CMake project through cmake/AudioProcessing.cmake.\n'
