#!/bin/sh
set -eu
set -f

if test "$#" -lt 2 || test "$#" -gt 4; then
    printf '%s\n' 'Usage: sh cmake/build-mobile-dependencies.sh ios-simulator|android PREFIX [BUILD_ROOT] [MIN_VERSION]' >&2
    exit 2
fi
mode=$1
prefix_arg=$2
build_root_arg=${3:-}
minimum=${4:-}
test "$mode" = ios-simulator || test "$mode" = android || {
    printf '%s\n' 'First argument must be ios-simulator or android.' >&2
    exit 2
}
prefix=$(CDPATH='' cd "$prefix_arg" 2>/dev/null && pwd || { mkdir -p "$prefix_arg"; CDPATH='' cd "$prefix_arg"; pwd; })
build_root=${build_root_arg:-"$prefix/.build"}
build_root=$(CDPATH='' cd "$build_root" 2>/dev/null && pwd || { mkdir -p "$build_root"; CDPATH='' cd "$build_root"; pwd; })
mkdir -p "$build_root/sources"

for tool in cmake ninja make curl shasum tar ar; do
    command -v "$tool" >/dev/null || { printf '%s\n' "$tool is required." >&2; exit 2; }
done

if test "$mode" = ios-simulator; then
    config_revision=3
    for tool in xcrun lipo; do command -v "$tool" >/dev/null || { printf '%s\n' "$tool is required." >&2; exit 2; }; done
    deployment_target=${minimum:-17.0}
    sdk=${IOS_SDK:-iphonesimulator}
    architecture=${IOS_ARCH:-$(uname -m)}
    test "$sdk" = iphonesimulator || { printf '%s\n' 'This script currently supports only the iOS Simulator SDK.' >&2; exit 2; }
    case "$architecture" in arm64|x86_64) ;; *) printf '%s\n' 'IOS_ARCH must be arm64 or x86_64.' >&2; exit 2 ;; esac
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
    clang=$(xcrun --sdk "$sdk" --find clang)
    clangxx=$(xcrun --sdk "$sdk" --find clang++)
    target="${architecture}-apple-ios${deployment_target}-simulator"
    openssl_target="iossimulator-${architecture}-xcrun"
    marker="$prefix/.squadspeak-ios-dependencies"
    platform_label='iOS Simulator'
    archive_tool=ar
else
    config_revision=6
    api=${minimum:-28}
    case "$api" in ''|*[!0-9]*) printf '%s\n' 'Android API must be numeric.' >&2; exit 2 ;; esac
    architecture=${MOBILE_ARCH:-$(uname -m)}
    case "$architecture" in arm64|x86_64) ;; *) printf '%s\n' 'MOBILE_ARCH must be arm64 or x86_64.' >&2; exit 2 ;; esac
    ndk=${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}
    test -d "$ndk" || { printf '%s\n' 'ANDROID_NDK_ROOT or ANDROID_NDK_HOME must point to an Android NDK.' >&2; exit 2; }
    ndk_version=$(sed -n 's/^Pkg.Revision[[:space:]]*=[[:space:]]*//p' "$ndk/source.properties")
    test -n "$ndk_version" || { printf '%s\n' "NDK source.properties has no Pkg.Revision: $ndk" >&2; exit 2; }
    case "$(uname -s)" in
        Darwin) host_tag=darwin-x86_64 ;;
        Linux) host_tag=linux-x86_64 ;;
        *) printf '%s\n' 'Unsupported host OS for Android NDK.' >&2; exit 2 ;;
    esac
    toolchain="$ndk/toolchains/llvm/prebuilt/$host_tag"
    test -x "$toolchain/bin/clang" || { printf '%s\n' "Unsupported NDK host toolchain: $toolchain" >&2; exit 2; }
    android_arch=$architecture
    test "$architecture" = arm64 && android_arch=arm64-v8a
    target_arch=$architecture
    test "$architecture" = arm64 && target_arch=aarch64
    clang="$toolchain/bin/${target_arch}-linux-android${api}-clang"
    clangxx="$toolchain/bin/${target_arch}-linux-android${api}-clang++"
    test -x "$clang" || { printf '%s\n' "NDK has no compiler for $architecture API $api: $clang" >&2; exit 2; }
    sysroot="$toolchain/sysroot"
    llvm_readelf="$toolchain/bin/llvm-readelf"
    archive_tool="$toolchain/bin/llvm-ar"
    test -x "$llvm_readelf" || { printf '%s\n' "NDK has no llvm-readelf: $llvm_readelf" >&2; exit 2; }
    target="${target_arch}-linux-android${api}"
    openssl_target=android-arm64
    test "$architecture" = x86_64 && openssl_target=android-x86_64
    marker="$prefix/.squadspeak-android-dependencies"
    platform_label="Android $architecture API $api"
fi

if test "$mode" = ios-simulator; then
    identity=$(cat <<EOF2
sdk=$sdk
architecture=$architecture
deployment_target=$deployment_target
config_revision=$config_revision
sysroot=$sysroot
EOF2
)
else
    identity=$(cat <<EOF2
mode=$mode
architecture=$architecture
android_api=$api
ndk=$ndk_version
config_revision=$config_revision
EOF2
)
fi

headers='include/openssl/ssl.h include/opus/opus.h include/samplerate.h
include/libavcodec/avcodec.h include/libavutil/avutil.h include/libswscale/swscale.h
include/libavformat/avformat.h include/libswresample/swresample.h'
if test "$mode" = android; then
    libraries='libcrypto_3.so libssl_3.so libopus.a libsamplerate.a'
else
    libraries='libcrypto.a libssl.a libopus.a libsamplerate.a
libavcodec.a libavutil.a libswscale.a libavformat.a libswresample.a'
fi

validate_static_library() {
    library=$1
    test -f "$library" || return 1
    check_dir=$(mktemp -d "${TMPDIR:-/tmp}/squad-mobile-library.XXXXXX")
    member=$("$archive_tool" -t "$library" | awk '$0 !~ /^(__\.SYMDEF|\/)/ { print; exit }')
    test -n "$member" || { rm -rf "$check_dir"; return 1; }
    if test "$mode" = ios-simulator; then
        lipo -verify_arch "$architecture" "$library" >/dev/null 2>&1 || { rm -rf "$check_dir"; return 1; }
        if ! (cd "$check_dir" && "$archive_tool" -x "$library" "$member" && xcrun vtool -show-build "$check_dir/$member" 2>/dev/null | grep -q 'platform IOSSIMULATOR'); then
            rm -rf "$check_dir"; return 1
        fi
    else
        expected='AArch64'
        test "$architecture" = x86_64 && expected='X86-64'
        if ! (cd "$check_dir" && "$archive_tool" -x "$library" "$member" && "$llvm_readelf" -h "$member" 2>/dev/null | grep -q "Machine:.*$expected"); then
            rm -rf "$check_dir"; return 1
        fi
    fi
    rm -rf "$check_dir"
}
validate_shared_library() {
    library=$1
    test -f "$library" || return 1
    expected='AArch64'
    test "$architecture" = x86_64 && expected='X86-64'
    test -n "${llvm_readelf:-}" && test -x "$llvm_readelf" || return 1
    "$llvm_readelf" -h "$library" 2>/dev/null | grep -q "Machine:.*$expected" || return 1
    "$llvm_readelf" -d "$library" | grep -Fq "Library soname: [$(basename "$library")]" || return 1
    if test "$(basename "$library")" = libssl_3.so; then
        "$llvm_readelf" -d "$library" | grep -Fq 'Shared library: [libcrypto_3.so]' || return 1
    fi
}
validate_prefix() {
    for header in $headers; do
        test -f "$prefix/$header" || return 1
    done
    for library_name in $libraries; do
        case "$library_name" in
            *.so) validate_shared_library "$prefix/lib/$library_name" || return 1 ;;
            *) validate_static_library "$prefix/lib/$library_name" || return 1 ;;
        esac
    done
}

if test -f "$marker"; then
    test "$(cat "$marker")" = "$identity" || { printf '%s\n' "Dependency prefix identity mismatch: $prefix" >&2; exit 2; }
    validate_prefix || { printf '%s\n' "Marked dependency prefix failed archive validation: $prefix" >&2; exit 2; }
    printf '%s\n' "$platform_label dependency prefix already valid: $prefix"
    exit 0
fi
outputs=$headers
for library_name in $libraries; do
    outputs="$outputs lib/$library_name"
done
for output in $outputs; do
    if test -e "$prefix/$output"; then
        printf '%s\n' "Unmarked dependency prefix contains output: $prefix" >&2
        printf '%s\n' 'Use a fresh prefix after an interrupted or architecture-switched build.' >&2
        exit 2
    fi
done

fetch() {
    name=$1; url=$2; digest=$3
    archive="$build_root/sources/$name"
    cache=${MOBILE_SOURCE_CACHE:-}
    if test ! -f "$archive" && test -n "$cache" && test -f "$cache/$name"; then cp "$cache/$name" "$archive"; fi
    if test ! -f "$archive"; then
        curl --fail --location --connect-timeout 20 --max-time 180 --retry 3 --output "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 --check
    directory="$build_root/sources/${name%.tar.gz}"; directory="${directory%.tar.xz}"
    if test ! -d "$directory"; then tar -xf "$archive" -C "$build_root/sources"; fi
}
fetch openssl-3.6.5.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98
fetch opus-1.6.1.tar.gz https://distfiles.macports.org/libopus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
fetch libsamplerate-0.2.2.tar.xz https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz 3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893
fetch ffmpeg-7.1.5.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

openssl_source="$build_root/sources/openssl-3.6.5"
(
    cd "$openssl_source"
    if test "$mode" = ios-simulator; then
        CFLAGS="-O3 -isysroot $sysroot -mios-simulator-version-min=$deployment_target" \
        LDFLAGS="-isysroot $sysroot -mios-simulator-version-min=$deployment_target" \
            ./Configure "$openssl_target" no-shared no-tests --prefix="$prefix" --libdir=lib
    else
        # OpenSSL's supported variant mechanism sets both filenames and SONAMEs.
        # Android must not confuse these libraries with its private system TLS.
        cat > "$build_root/openssl-android.conf" <<EOF2
my %targets = (
    "squad-$openssl_target" => {
        inherit_from => [ "$openssl_target" ],
        shlib_variant => "_3",
    },
);
EOF2
        export PATH="$toolchain/bin:$PATH"
        ./Configure --config="$build_root/openssl-android.conf" "squad-$openssl_target" shared no-tests \
            "-D__ANDROID_API__=$api" --prefix="$prefix" --libdir=lib
    fi
    make -j3
    make install_sw
)

toolchain="$build_root/mobile-toolchain.cmake"
if test "$mode" = ios-simulator; then
cat > "$toolchain" <<EOF2
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_SYSROOT $sdk)
set(CMAKE_OSX_ARCHITECTURES $architecture)
set(CMAKE_OSX_DEPLOYMENT_TARGET $deployment_target)
set(CMAKE_C_COMPILER "$clang")
set(CMAKE_CXX_COMPILER "$clangxx")
EOF2
else
cat > "$toolchain" <<EOF2
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_ANDROID_NDK "$ndk")
set(CMAKE_ANDROID_ARCH_ABI $android_arch)
set(CMAKE_ANDROID_API $api)
set(CMAKE_C_COMPILER "$clang")
set(CMAKE_CXX_COMPILER "$clangxx")
EOF2
fi

for dependency in opus-1.6.1 libsamplerate-0.2.2; do
    if test "$dependency" = opus-1.6.1; then output="$prefix/lib/libopus.a"; header="$prefix/include/opus/opus.h"; else output="$prefix/lib/libsamplerate.a"; header="$prefix/include/samplerate.h"; fi
    build="$build_root/$dependency-$architecture-build"
    if test ! -f "$output" || test ! -f "$header" || ! validate_static_library "$output"; then
        cmake -S "$build_root/sources/$dependency" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DLIBSAMPLERATE_EXAMPLES=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        cmake --build "$build" --parallel 2; cmake --install "$build"
    fi
done

ffmpeg_source="$build_root/sources/ffmpeg-7.1.5"
if test "$mode" = ios-simulator; then
    ffmpeg_cpu_flags=""
    if test "$architecture" = x86_64; then ffmpeg_cpu_flags="--disable-x86asm --disable-runtime-cpudetect --disable-mmx --disable-mmxext --disable-sse --disable-sse2 --disable-sse3 --disable-sse4 --disable-sse42"; fi
    (cd "$ffmpeg_source" && ./configure --prefix="$prefix" --target-os=darwin --arch="$architecture" --enable-cross-compile --cc="$clang" --sysroot="$sysroot" --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-shared --enable-static --enable-network --enable-avformat --enable-swresample --disable-avfilter --disable-avdevice --enable-videotoolbox --enable-securetransport --enable-pic $ffmpeg_cpu_flags --extra-cflags="-target $target -isysroot $sysroot" --extra-ldflags="-target $target -isysroot $sysroot" && make -j3 && make install)
else
    ffmpeg_arch=$architecture; test "$architecture" = arm64 && ffmpeg_arch=aarch64
    (cd "$ffmpeg_source" && PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH= ./configure --prefix="$prefix" --target-os=android --arch="$ffmpeg_arch" --enable-cross-compile --cc="$clang" --ar="$ndk/toolchains/llvm/prebuilt/$host_tag/bin/llvm-ar" --ranlib="$ndk/toolchains/llvm/prebuilt/$host_tag/bin/llvm-ranlib" --strip="$ndk/toolchains/llvm/prebuilt/$host_tag/bin/llvm-strip" --disable-programs --disable-doc --disable-debug --disable-autodetect --enable-shared --disable-static --enable-network --enable-avformat --enable-swresample --disable-avfilter --disable-avdevice --enable-openssl --enable-version3 --enable-mediacodec --enable-jni --enable-pic --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib -lssl_3 -lcrypto_3 -llog -landroid -ldl" && make install-headers)
fi
validate_prefix || { printf '%s\n' "Final $platform_label archive validation failed." >&2; exit 1; }
printf '%s\n' "$identity" > "$marker"
printf '%s dependency prefix ready: %s\n' "$platform_label" "$prefix"
