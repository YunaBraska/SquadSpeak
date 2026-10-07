cmake_minimum_required(VERSION 3.24)
# Match the desktop Qt baseline instead of inheriting the build machine's OS.
# Explicit deployment targets and mobile toolchains keep their own settings.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET
        AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
include(cmake/Version.cmake)
project(SquadSpeak VERSION ${SQUADSPEAK_VERSION} LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX)
endif()

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Quick QuickControls2 Multimedia Network Sql LinguistTools)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
if(WIN32)
    set(SQUADSPEAK_AUDIO_DEPS_ROOT "" CACHE PATH
        "Staged Opus and libsamplerate headers and libraries")
    if(NOT SQUADSPEAK_AUDIO_DEPS_ROOT)
        message(FATAL_ERROR "Windows requires -DSQUADSPEAK_AUDIO_DEPS_ROOT=<staged audio SDK>.")
    endif()
    find_path(squad_opus_include_dir opus.h
        PATHS "${SQUADSPEAK_AUDIO_DEPS_ROOT}/include/opus" NO_DEFAULT_PATH REQUIRED)
    find_library(squad_opus_library NAMES opus opus-0
        PATHS "${SQUADSPEAK_AUDIO_DEPS_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_path(squad_samplerate_include_dir samplerate.h
        PATHS "${SQUADSPEAK_AUDIO_DEPS_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(squad_samplerate_library NAMES samplerate samplerate-0
        PATHS "${SQUADSPEAK_AUDIO_DEPS_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(PkgConfig::OPUS INTERFACE IMPORTED)
    set_target_properties(PkgConfig::OPUS PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
        "${squad_opus_include_dir}" INTERFACE_LINK_LIBRARIES "${squad_opus_library}")
    add_library(PkgConfig::SAMPLERATE INTERFACE IMPORTED)
    set_target_properties(PkgConfig::SAMPLERATE PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
        "${squad_samplerate_include_dir}" INTERFACE_LINK_LIBRARIES "${squad_samplerate_library}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus>=1.3)
    pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(XINPUT REQUIRED IMPORTED_TARGET x11 xi)
    set(squad_ptt_platform_libraries PkgConfig::XINPUT)
elseif(APPLE)
    set(squad_ptt_platform_libraries "-framework CoreGraphics" "-framework CoreFoundation")
elseif(WIN32)
    set(squad_ptt_platform_libraries user32)
endif()
qt_standard_project_setup(REQUIRES 6.10)

include(FetchContent)
include(cmake/AudioProcessing.cmake)
include(cmake/Media.cmake)

# Backport the portal/session and GC fixes into the distribution's exact Qt
# ABI. Only the corrected modules are bundled; no mismatched Qt replacement.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Qt6_VERSION VERSION_LESS "6.12.0")
    if(NOT Qt6_VERSION VERSION_EQUAL "6.10.2")
        message(FATAL_ERROR "Linux requires Qt 6.10.2 with the bundled fixes, or Qt 6.12 or newer.")
    endif()
    FetchContent_Declare(qt_capture
        URL https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtmultimedia-everywhere-src-6.10.2.tar.xz
        URL_HASH SHA256=93f7ef0106fbd731165a2723f3e436c911fc5e6880f5bc987b55516c20833e2b
        SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(qt_qml
        URL https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtdeclarative-everywhere-src-6.10.2.tar.xz
        URL_HASH SHA256=a249914ff66cdcdbf0df8b5ffad997a2ee6dce01cc17d43c6cc56fdc1d0f4b0f
        SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(qt_capture qt_qml)
    find_package(Git REQUIRED)
    get_target_property(qt_qmake Qt6::qmake LOCATION)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_PREFIX
        OUTPUT_VARIABLE qt_prefix OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_LIBS
        OUTPUT_VARIABLE qt_libs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(RELATIVE_PATH qt_runtime_libdir "${qt_prefix}" "${qt_libs}")
    foreach(module IN ITEMS capture qml)
        if(module STREQUAL "capture")
            set(component Multimedia)
            set(patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qt-wayland.patch")
        else()
            set(component Qml)
            set(patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qt-qml-gc.patch")
        endif()
        set(source "${qt_${module}_SOURCE_DIR}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
            WORKING_DIRECTORY "${source}" RESULT_VARIABLE unpatched OUTPUT_QUIET ERROR_QUIET)
        if(unpatched EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
                WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
        else()
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
                WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
        endif()
        # Standalone modules inherit the SDK layout, including multiarch dirs.
        set(libdir "${CMAKE_CURRENT_BINARY_DIR}/qt-${module}/${qt_runtime_libdir}")
        set(library "${libdir}/libQt6${component}.so.6.10.2")
        list(APPEND qt_runtime_libraries "${library}")
        set(module_build_options
            BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/qt-${module}"
            CMAKE_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF
                "-DQt6_DIR=${Qt6_DIR}" -DFEATURE_gstreamer=OFF
                "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}" "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
                "-DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
            BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target ${component} --parallel 4
            INSTALL_COMMAND "" BUILD_BYPRODUCTS "${library}")
        if(module STREQUAL "capture")
            ExternalProject_Add(squad_qt_capture_build
                SOURCE_DIR "${qt_capture_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
                ${module_build_options})
        else()
            ExternalProject_Add(squad_qt_qml_build
                SOURCE_DIR "${qt_qml_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
                ${module_build_options})
        endif()
        get_target_property(configurations Qt6::${component} IMPORTED_CONFIGURATIONS)
        foreach(configuration IN LISTS configurations)
            set_property(TARGET Qt6::${component} PROPERTY "IMPORTED_LOCATION_${configuration}" "${library}")
        endforeach()
        set_property(TARGET Qt6::${component} PROPERTY IMPORTED_LOCATION "${library}")
        add_dependencies(Qt6::${component} squad_qt_${module}_build)
        list(PREPEND CMAKE_BUILD_RPATH "${libdir}")
    endforeach()
endif()

# Qt Multimedia and the app must resolve one FFmpeg ABI. macOS builds the
# matching version with VideoToolbox lifetime fixes; Windows uses its
# staged Qt SDK and Linux uses the distribution packages.
add_library(squad_ffmpeg INTERFACE)
if(APPLE)
    FetchContent_Declare(ffmpeg
        URL https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz
        URL_HASH SHA256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(ffmpeg)
    # FFmpeg 04ff86d0355a58c3d02dbf562ccb708942a29435: a dropped
    # VideoToolbox frame has no sample buffer, but still owns its queue node.
    set(ffmpeg_encoder "${ffmpeg_SOURCE_DIR}/libavcodec/videotoolboxenc.c")
    file(READ "${ffmpeg_encoder}" ffmpeg_source)
    set(ffmpeg_dropped_before "    if (!sample_buffer) {\n        return;\n    }")
    set(ffmpeg_dropped_after "    if (!sample_buffer) {\n        vtenc_free_buf_node(info);\n        return;\n    }")
    # Extradata initialization owns its node until EncodeFrame succeeds.
    # Failed session creation must free it too; transferred nodes are NULL.
    set(ffmpeg_init_before "    if (!status)\n        vtenc_free_buf_node(node);")
    set(ffmpeg_init_after "    if (node)\n        vtenc_free_buf_node(node);")
    foreach(cleanup IN ITEMS dropped init)
        string(FIND "${ffmpeg_source}" "${ffmpeg_${cleanup}_after}" ffmpeg_patched)
        if(ffmpeg_patched LESS 0)
            string(FIND "${ffmpeg_source}" "${ffmpeg_${cleanup}_before}" ffmpeg_match)
            if(ffmpeg_match LESS 0)
                message(FATAL_ERROR "Unexpected pinned FFmpeg ${cleanup} cleanup")
            endif()
            string(REPLACE "${ffmpeg_${cleanup}_before}" "${ffmpeg_${cleanup}_after}" ffmpeg_source "${ffmpeg_source}")
            file(WRITE "${ffmpeg_encoder}" "${ffmpeg_source}")
        endif()
    endforeach()
    set(squad_ffmpeg_library_dir "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/lib")
    set(ffmpeg_prefix "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")
    # Prefer the patched ABI-compatible runtime over the Qt SDK copy in tests
    # and build-tree apps as well as the installed bundle.
    list(PREPEND CMAKE_BUILD_RPATH "${squad_ffmpeg_library_dir}")
    set(ffmpeg_arch "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT ffmpeg_arch)
        set(ffmpeg_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT ffmpeg_arch MATCHES "^(arm64|x86_64)$")
        message(FATAL_ERROR "Build one macOS architecture at a time: arm64 or x86_64.")
    endif()
    set(ffmpeg_flags "-arch ${ffmpeg_arch} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET} ${CMAKE_C_FLAGS}")
    if(CMAKE_OSX_SYSROOT)
        string(APPEND ffmpeg_flags " -isysroot ${CMAKE_OSX_SYSROOT}")
    endif()
    file(MAKE_DIRECTORY "${ffmpeg_prefix}/include")
    set(ffmpeg_libraries)
    foreach(library IN ITEMS avcodec.61 avutil.59 swscale.8 avformat.61 swresample.5)
        string(REPLACE "." "_" library_target "squad_${library}")
        set(library_path "${squad_ffmpeg_library_dir}/lib${library}.dylib")
        add_library(${library_target} SHARED IMPORTED)
        set_target_properties(${library_target} PROPERTIES IMPORTED_LOCATION "${library_path}")
        list(APPEND ffmpeg_libraries "${library_path}")
        target_link_libraries(squad_ffmpeg INTERFACE ${library_target})
    endforeach()
    find_program(SQUAD_MAKE make REQUIRED)
    ExternalProject_Add(squad_ffmpeg_build
        SOURCE_DIR "${ffmpeg_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND "${ffmpeg_SOURCE_DIR}/configure" "--prefix=${ffmpeg_prefix}"
            --install-name-dir=@rpath --extra-version=squadspeak.2
            --disable-programs --disable-doc --disable-debug --enable-network
            --disable-lzma --enable-pic --disable-vulkan --disable-v4l2-m2m
            --disable-decoder=truemotion1 --disable-avdevice --disable-avfilter
            --enable-shared --disable-static --disable-autodetect
            --enable-videotoolbox --enable-audiotoolbox --enable-securetransport
            --enable-zlib --enable-bzlib --enable-iconv --extra-libs=-liconv
            "--cc=${CMAKE_C_COMPILER}" "--arch=${ffmpeg_arch}"
            "--extra-cflags=${ffmpeg_flags}" "--extra-ldflags=${ffmpeg_flags}"
            "--host-cflags=${ffmpeg_flags}" "--host-ldflags=${ffmpeg_flags}"
        BUILD_COMMAND "${SQUAD_MAKE}" -j3
        INSTALL_COMMAND "${SQUAD_MAKE}" install
        BUILD_BYPRODUCTS ${ffmpeg_libraries})
    add_dependencies(squad_ffmpeg squad_ffmpeg_build)
    target_include_directories(squad_ffmpeg INTERFACE "${ffmpeg_prefix}/include")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Use the same distro development packages that own Qt's FFmpeg plugin.
    # This keeps headers and ABI libraries in one package-manager namespace.
    pkg_check_modules(SQUAD_FFMPEG_AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(SQUAD_FFMPEG_AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(SQUAD_FFMPEG_SWSCALE REQUIRED IMPORTED_TARGET libswscale)
    target_link_libraries(squad_ffmpeg INTERFACE
        PkgConfig::SQUAD_FFMPEG_AVCODEC
        PkgConfig::SQUAD_FFMPEG_AVUTIL
        PkgConfig::SQUAD_FFMPEG_SWSCALE)
elseif(WIN32)
    set(SQUADSPEAK_FFMPEG_ROOT "" CACHE PATH
        "Staged FFmpeg headers and import libraries matching the Qt Multimedia DLLs")
    if(NOT SQUADSPEAK_FFMPEG_ROOT)
        message(FATAL_ERROR
            "Windows requires -DSQUADSPEAK_FFMPEG_ROOT=<staged Qt FFmpeg SDK>. "
            "Use tests/windows-ci.ps1 to stage official FFmpeg headers and import libraries for Qt's DLLs.")
    endif()
    find_path(squad_ffmpeg_include_dir libavcodec/avcodec.h
        PATHS "${SQUADSPEAK_FFMPEG_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(squad_ffmpeg_avcodec avcodec PATHS "${SQUADSPEAK_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(squad_ffmpeg_avutil avutil PATHS "${SQUADSPEAK_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(squad_ffmpeg_swscale swscale PATHS "${SQUADSPEAK_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_include_directories(squad_ffmpeg INTERFACE "${squad_ffmpeg_include_dir}")
    target_link_libraries(squad_ffmpeg INTERFACE
        "${squad_ffmpeg_avcodec}" "${squad_ffmpeg_avutil}" "${squad_ffmpeg_swscale}")
endif()

function(squad_add_keychain)
    set(BUILD_WITH_QT6 ON)
    set(BUILD_TRANSLATIONS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(qtkeychain
        URL https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/0.16.0.tar.gz
        URL_HASH SHA256=3be26ec4ae30eecf0c2ff7572ba83799791b157c76e15a05ef35f23dc25e4054
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(qtkeychain)
    # Keep narrow corrections in build-local copies of the pinned dependency.
    # Refuse an unreviewed upstream revision instead of silently misapplying them.
    set(corrected_sources keychain.cpp)
    if(APPLE)
        list(APPEND corrected_sources keychain_apple.mm)
    endif()
    get_target_property(keychain_sources qt6keychain SOURCES)
    foreach(source IN LISTS corrected_sources)
        if(source STREQUAL "keychain.cpp")
            # QObject::destroyed runs after Job's destructor. Downcasting the
            # remaining QObject to Job is undefined even just to disconnect it.
            set(expected_hash "ba0c2a47d29b21a063a932547c46a11125cf3c3b0b877abadb3c470d9d59311f")
            set(before "Job *job = static_cast<Job *>(object);")
            set(after "QObject *job = object;")
        else()
            # Identity loading blocks server readiness; background QoS can be
            # deferred indefinitely on macOS. Change only these short operations.
            set(expected_hash "e31ba2930705f1e1515fa733a1b8ca2898816f1a24a2d14486c86cccb1142a4e")
            set(before "DISPATCH_QUEUE_PRIORITY_BACKGROUND")
            set(after "QOS_CLASS_USER_INITIATED")
        endif()
        set(original "${qtkeychain_SOURCE_DIR}/qtkeychain/${source}")
        file(SHA256 "${original}" source_hash)
        if(NOT source_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Review the QtKeychain correction for ${source} at this source version.")
        endif()
        file(READ "${original}" code)
        string(REPLACE "${before}" "${after}" code "${code}")
        if(source STREQUAL "keychain_apple.mm")
            # Blocks preserve C++ reference parameters as references. Own the
            # implicitly shared inputs until a canceled job's native work ends.
            string(REPLACE "const QString &service, const QString &key" "QString service, QString key" code "${code}")
            string(REPLACE "const QByteArray &data," "QByteArray data," code "${code}")
        endif()
        set(patched "${qtkeychain_BINARY_DIR}/${source}")
        file(CONFIGURE OUTPUT "${patched}" CONTENT "${code}" @ONLY)
        list(REMOVE_ITEM keychain_sources "${source}")
        list(APPEND keychain_sources "${patched}")
    endforeach()
    set_property(TARGET qt6keychain PROPERTY SOURCES "${keychain_sources}")
    target_include_directories(qt6keychain PRIVATE "${qtkeychain_SOURCE_DIR}/qtkeychain")
    # This is a private static dependency. Installing the app must not install
    # its development headers, CMake exports or translation tooling.
    set_property(DIRECTORY "${qtkeychain_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endfunction()
squad_add_keychain()

# v0.2 plus its immediate upstream ARM/SSE build correction. Model and code
# are pinned separately; the resulting library needs no runtime downloads.
FetchContent_Declare(rnnoise
    URL https://github.com/xiph/rnnoise/archive/372f7b4b76cde4ca1ec4605353dd17898a99de38.tar.gz
    URL_HASH SHA256=40ff1568af151959933699fcbf2db3ee3c62fa9559557dbf61a65e7a12cd335d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(rnnoise_model
    URL https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz
    URL_HASH SHA256=4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rnnoise rnnoise_model)
add_library(squad_rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c ${rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c)
set_target_properties(squad_rnnoise PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF)
target_include_directories(squad_rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include
    PRIVATE ${rnnoise_SOURCE_DIR}/src ${rnnoise_model_SOURCE_DIR}/src)
target_compile_definitions(squad_rnnoise PRIVATE RNNOISE_BUILD)
# Keep inference optimized in debug builds as well; do not enable fast-math.
target_compile_options(squad_rnnoise PRIVATE $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-O2> $<$<C_COMPILER_ID:MSVC>:/O2>)
if(UNIX)
    target_link_libraries(squad_rnnoise PRIVATE m)
endif()

# Qt 6.9.0 advertises a direct AGL link even when the active SDK has removed
# its stub. QtGui itself has no AGL symbol imports. Keep its runtime dependency
# untouched; only remove the unavailable direct link from the imported target.
if(APPLE AND Qt6_VERSION VERSION_EQUAL "6.9.0" AND TARGET WrapOpenGL::WrapOpenGL
        AND IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}"
        AND NOT EXISTS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/AGL.framework")
    get_target_property(squad_opengl_links WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER squad_opengl_links EXCLUDE REGEX "(^-framework AGL$|/AGL\\.framework$)")
    set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${squad_opengl_links}")
endif()

add_library(squad_audio src/audio_processor.cpp src/audio_profiles.cpp src/voice_mixer.cpp src/echo_canceller.cpp src/output_monitor.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(squad_audio PRIVATE src/output_monitor_mac.mm)
    set_source_files_properties(src/output_monitor_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(squad_audio PRIVATE "-framework CoreAudio" "-framework Foundation")
elseif(WIN32)
    target_sources(squad_audio PRIVATE src/output_monitor_windows.cpp)
    target_link_libraries(squad_audio PRIVATE ole32 mmdevapi)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
    target_sources(squad_audio PRIVATE src/output_monitor_linux.cpp)
    target_link_libraries(squad_audio PRIVATE PkgConfig::PULSE)
endif()
qt_add_resources(squad_audio event_sounds PREFIX /qt/qml/SquadSpeak/ui BASE ui
    FILES ui/sounds/join.wav ui/sounds/leave.wav ui/sounds/kick.wav ui/sounds/ban.wav
          ui/sounds/message.wav ui/sounds/announcement.wav)
target_include_directories(squad_audio PUBLIC src)
target_link_libraries(squad_audio PUBLIC Qt6::Core PkgConfig::OPUS PkgConfig::SAMPLERATE squad_rnnoise PRIVATE squad_apm)
target_compile_options(squad_audio PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)

add_library(squad_session src/voice_session.cpp src/voice_session.hpp src/tls_identity.cpp src/tls_identity.hpp src/local_channel.cpp src/local_channel_chat.cpp src/local_channel_control.cpp src/local_channel_discovery.cpp src/local_channel_video.cpp src/local_channel.hpp src/media_transport.cpp src/media_transport.hpp src/chat_history.cpp src/chat_history.hpp src/license.cpp src/license.hpp)
target_include_directories(squad_session PUBLIC src)
target_compile_definitions(squad_session PUBLIC SQUADSPEAK_VERSION="${PROJECT_VERSION}")
option(SQUADSPEAK_STORE_BUILD "Build a free-only store distribution" OFF)
set(SQUADSPEAK_PURCHASE_URL "" CACHE STRING "Public annual-pass checkout URL")
foreach(field STORE PRODUCT VARIANT)
    set(SQUADSPEAK_LICENSE_${field}_ID "0" CACHE STRING "Public license ${field} ID")
    if(NOT SQUADSPEAK_LICENSE_${field}_ID MATCHES "^(0|[1-9][0-9]*)$"
            OR SQUADSPEAK_LICENSE_${field}_ID GREATER 2147483647)
        message(FATAL_ERROR "Invalid public license ${field} ID")
    endif()
    target_compile_definitions(squad_session PUBLIC SQUADSPEAK_LICENSE_${field}_ID=${SQUADSPEAK_LICENSE_${field}_ID})
endforeach()
if((SQUADSPEAK_LICENSE_STORE_ID OR SQUADSPEAK_LICENSE_PRODUCT_ID OR SQUADSPEAK_LICENSE_VARIANT_ID)
        AND NOT (SQUADSPEAK_LICENSE_STORE_ID AND SQUADSPEAK_LICENSE_PRODUCT_ID AND SQUADSPEAK_LICENSE_VARIANT_ID))
    message(FATAL_ERROR "Configure all three public license IDs together")
endif()
if(SQUADSPEAK_PURCHASE_URL AND NOT SQUADSPEAK_PURCHASE_URL MATCHES "^https://[A-Za-z0-9.-]+\\.lemonsqueezy\\.com/[A-Za-z0-9/_-]+$")
    message(FATAL_ERROR "Use a public Lemon Squeezy HTTPS checkout URL")
endif()
target_compile_definitions(squad_session PUBLIC SQUADSPEAK_STORE_BUILD=$<BOOL:${SQUADSPEAK_STORE_BUILD}>
    SQUADSPEAK_PURCHASE_URL="${SQUADSPEAK_PURCHASE_URL}")
target_link_libraries(squad_session PUBLIC Qt6::Core Qt6::Network Qt6::Sql PRIVATE OpenSSL::Crypto qt6keychain PkgConfig::OPUS datachannel-static)
target_compile_options(squad_session PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)
file(GLOB squad_translation_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.qml")
file(GLOB squad_catalogs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n/squadspeak_*.ts")
set(squad_languages)
foreach(catalog IN LISTS squad_catalogs)
    get_filename_component(language "${catalog}" NAME_WE)
    string(REGEX REPLACE "^squadspeak_" "" language "${language}")
    list(APPEND squad_languages "${language}")
endforeach()
qt_add_translations(squad_session
    TS_FILES ${squad_catalogs}
    SOURCE_TARGETS squad_session
    SOURCES ${squad_translation_sources}
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none
    IMMEDIATE_CALL)
get_filename_component(squad_qt_translations "${QT6_INSTALL_TRANSLATIONS}" ABSOLUTE BASE_DIR "${QT6_INSTALL_PREFIX}")
set(squad_standard_catalogs)
foreach(language IN LISTS squad_languages)
    set(standard_language "${language}")
    if(language STREQUAL "zh")
        set(standard_language zh_CN)
    elseif(language STREQUAL "pt")
        set(standard_language pt_BR)
    endif()
    foreach(module qtbase qtdeclarative)
        set(standard_catalog "${squad_qt_translations}/${module}_${standard_language}.qm")
        if(EXISTS "${standard_catalog}")
            set_source_files_properties("${standard_catalog}" PROPERTIES
                QT_RESOURCE_ALIAS "${module}_${language}.qm")
            list(APPEND squad_standard_catalogs "${standard_catalog}")
        endif()
    endforeach()
endforeach()
if(squad_standard_catalogs)
    qt_add_resources(squad_session standard_translations PREFIX /i18n
        BASE "${squad_qt_translations}" FILES ${squad_standard_catalogs})
    target_compile_definitions(squad_session PRIVATE SQUAD_STANDARD_TRANSLATIONS)
endif()

add_library(squad_content src/chat_content.cpp src/chat_content.hpp)
target_include_directories(squad_content PUBLIC src)
target_link_libraries(squad_content PUBLIC Qt6::Gui Qt6::Network)
target_link_libraries(squad_session PRIVATE squad_content)

add_library(squad_radio src/radio_player.cpp src/radio_player.hpp)
qt_add_resources(squad_radio radio_catalog PREFIX /radio BASE src FILES src/radio_catalog.json)
target_include_directories(squad_radio PUBLIC src)
target_link_libraries(squad_radio PUBLIC squad_audio squad_session Qt6::Multimedia)
target_compile_options(squad_radio PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)

add_library(squad_video src/video_codec.cpp src/video_codec.hpp src/screen_share.cpp src/screen_share.hpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(squad_video PRIVATE src/screen_capture_mac.mm)
    set_source_files_properties(src/screen_capture_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(squad_video PRIVATE squad_audio "-framework ScreenCaptureKit" "-framework CoreMedia" "-framework CoreVideo" "-framework CoreGraphics")
endif()
target_include_directories(squad_video PUBLIC src)
target_link_libraries(squad_video PUBLIC squad_session Qt6::Multimedia PRIVATE squad_ffmpeg)
if(WIN32)
    target_sources(squad_video PRIVATE src/screen_capture_windows.cpp)
    target_link_libraries(squad_video PRIVATE squad_audio ole32 mmdevapi user32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(squad_video PRIVATE src/screen_capture_linux.cpp)
    target_link_libraries(squad_video PRIVATE squad_audio PkgConfig::PULSE PkgConfig::XINPUT)
endif()
target_compile_options(squad_video PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)

add_executable(squad_image_worker src/image_worker.cpp)
target_link_libraries(squad_image_worker PRIVATE squad_content Qt6::Core)

# Keep production QML scanning inside the UI tree, away from tests and build outputs.
include(cmake/Updates.cmake)
add_subdirectory(ui)

if(APPLE)
    set_property(TARGET squad_image_worker PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")
    # Keep the install tree self-contained without requiring a signing
    # certificate or a DMG.
    # Qt's generated script delegates to the Qt installation's macdeployqt and
    # discovers the QML imports and linked frameworks from the built bundle.
    qt_generate_deploy_qml_app_script(
        TARGET squadspeak
        OUTPUT_SCRIPT squadspeak_deploy_script
        NO_TRANSLATIONS
        DEPLOY_TOOL_OPTIONS "-no-plugins" "-libpath=${squad_ffmpeg_library_dir}" "-libpath=${QT6_INSTALL_PREFIX}/lib"
            "-executable=squadspeak.app/Contents/MacOS/squad_image_worker")
    install(TARGETS squadspeak BUNDLE DESTINATION .)
    install(TARGETS squad_image_worker RUNTIME DESTINATION squadspeak.app/Contents/MacOS)
    install(FILES LICENSE DESTINATION squadspeak.app/Contents/Resources)
    if(TARGET squad_updater)
        # Preserve Sparkle's nested helper apps/XPC services before deployment
        # resolves the framework through the app's installed runtime path.
        install(DIRECTORY "${sparkle_SOURCE_DIR}/Sparkle.framework"
            DESTINATION squadspeak.app/Contents/Frameworks)
    endif()
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/third-party/"
        DESTINATION squadspeak.app/Contents/Resources/THIRD_PARTY_NOTICES
        FILES_MATCHING PATTERN "*.txt" PATTERN "*.spdx")
    get_filename_component(squad_qt_dbus_framework "${QT6_INSTALL_PREFIX}/lib/QtDBus.framework" REALPATH)
    install(DIRECTORY "${squad_qt_dbus_framework}/"
        DESTINATION squadspeak.app/Contents/Frameworks/QtDBus.framework)
    # macdeployqt copies every SQL driver when QtSql is linked, including
    # drivers for databases we do not use with unbundled vendor libraries.
    # Stage the runtime plugins first; deployment rewrites these existing dylibs.
    foreach(plugin_type platforms imageformats iconengines multimedia tls networkinformation styles platforminputcontexts)
        file(GLOB runtime_plugins "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/${plugin_type}/*.dylib")
        list(FILTER runtime_plugins EXCLUDE REGEX "_debug[.]dylib$")
        install(FILES ${runtime_plugins} DESTINATION "squadspeak.app/Contents/PlugIns/${plugin_type}")
    endforeach()
    install(FILES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/sqldrivers/libqsqlite.dylib"
        DESTINATION squadspeak.app/Contents/PlugIns/sqldrivers)
    # Clear Finder metadata before deployment. Keep linker signatures intact
    # while install_name_tool rewrites paths; refresh signatures afterwards.
    install(CODE [[
        cmake_policy(SET CMP0009 NEW)
        set(_squadspeak_app_predeploy "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/squadspeak.app")
        execute_process(COMMAND /usr/bin/xattr -dr com.apple.FinderInfo "${_squadspeak_app_predeploy}" ERROR_QUIET)
        execute_process(COMMAND /usr/bin/xattr -dr com.apple.ResourceFork "${_squadspeak_app_predeploy}" ERROR_QUIET)
    ]])
    install(SCRIPT ${squadspeak_deploy_script})
    # macdeployqt can prefer its SDK copy while inspecting plugin dependencies.
    # Replace each ABI library after path deployment, before signing. COPY_FILE
    # follows the build-tree symlink; the bundle needs only one file per ABI.
    foreach(ffmpeg_library IN LISTS ffmpeg_libraries)
        get_filename_component(ffmpeg_name "${ffmpeg_library}" NAME)
        install(CODE "file(COPY_FILE \"${ffmpeg_library}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/squadspeak.app/Contents/Frameworks/${ffmpeg_name}\" ONLY_IF_DIFFERENT)")
    endforeach()
    # Match deployed code to the app and remove obsolete SDK search paths
    # before refreshing signatures.
    install(CODE [[
        set(_squadspeak_app "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/squadspeak.app")
        execute_process(COMMAND /usr/bin/lipo -archs "${_squadspeak_app}/Contents/MacOS/squadspeak"
            OUTPUT_VARIABLE _squadspeak_arch OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        file(GLOB_RECURSE _squadspeak_binaries LIST_DIRECTORIES false "${_squadspeak_app}/Contents/*")
        foreach(_squadspeak_binary IN LISTS _squadspeak_binaries)
            if(IS_SYMLINK "${_squadspeak_binary}")
                continue()
            endif()
            file(READ "${_squadspeak_binary}" _squadspeak_magic LIMIT 4 HEX)
            if(NOT _squadspeak_magic MATCHES "^(cffaedfe|cefaedfe|cafebabe|bebafeca|cafebabf|bfbafeca)$")
                continue()
            endif()
            if(NOT _squadspeak_arch MATCHES " " AND _squadspeak_magic MATCHES "^(cafebabe|bebafeca|cafebabf|bfbafeca)$")
                execute_process(COMMAND /usr/bin/lipo "${_squadspeak_binary}" -thin "${_squadspeak_arch}"
                    -output "${_squadspeak_binary}" COMMAND_ERROR_IS_FATAL ANY)
            endif()
            execute_process(COMMAND /usr/bin/otool -l "${_squadspeak_binary}"
                OUTPUT_VARIABLE _squadspeak_load_commands COMMAND_ERROR_IS_FATAL ANY)
            string(REGEX MATCHALL "cmd LC_RPATH[^\n]*\n[^\n]*\n[^\n]*" _squadspeak_rpath_commands "${_squadspeak_load_commands}")
            list(REMOVE_DUPLICATES _squadspeak_rpath_commands)
            foreach(_squadspeak_rpath_command IN LISTS _squadspeak_rpath_commands)
                string(REGEX REPLACE ".*path (.*) \\(offset.*" "\\1" _squadspeak_rpath "${_squadspeak_rpath_command}")
                get_filename_component(_squadspeak_loader_dir "${_squadspeak_binary}" DIRECTORY)
                string(REPLACE "@loader_path" "${_squadspeak_loader_dir}" _squadspeak_search_path "${_squadspeak_rpath}")
                string(REPLACE "@executable_path" "${_squadspeak_app}/Contents/MacOS" _squadspeak_search_path "${_squadspeak_search_path}")
                cmake_path(IS_PREFIX _squadspeak_app "${_squadspeak_search_path}" NORMALIZE _squadspeak_internal_path)
                if(NOT _squadspeak_internal_path)
                    execute_process(COMMAND /usr/bin/install_name_tool -delete_rpath "${_squadspeak_rpath}" "${_squadspeak_binary}"
                        COMMAND_ERROR_IS_FATAL ANY)
                endif()
            endforeach()
        endforeach()
    ]])
    # Xcode 27 emits an ad-hoc linker signature. The install step rewrites
    # Mach-O paths while deploying, so refresh that signature without a
    # certificate; otherwise macOS terminates the installed executable.
    install(CODE [[
        set(_squadspeak_app "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/squadspeak.app")
        execute_process(COMMAND /usr/bin/xattr -dr com.apple.FinderInfo "${_squadspeak_app}")
        execute_process(COMMAND /usr/bin/xattr -dr com.apple.ResourceFork "${_squadspeak_app}")
        file(GLOB _squadspeak_frameworks LIST_DIRECTORIES true "${_squadspeak_app}/Contents/Frameworks/*.framework")
        foreach(_squadspeak_framework IN LISTS _squadspeak_frameworks)
            execute_process(COMMAND /usr/bin/xattr -d com.apple.FinderInfo "${_squadspeak_framework}" ERROR_QUIET)
            execute_process(COMMAND /usr/bin/xattr -d com.apple.ResourceFork "${_squadspeak_framework}" ERROR_QUIET)
            execute_process(COMMAND /usr/bin/codesign --remove-signature "${_squadspeak_framework}" ERROR_QUIET)
            execute_process(COMMAND /usr/bin/codesign --force --deep --sign - "${_squadspeak_framework}"
                RESULT_VARIABLE _squadspeak_framework_sign_result)
            if(NOT _squadspeak_framework_sign_result EQUAL 0)
                message(FATAL_ERROR "Could not ad-hoc sign deployed Qt framework")
            endif()
        endforeach()
        execute_process(COMMAND /usr/bin/xattr -d com.apple.FinderInfo "${_squadspeak_app}" ERROR_QUIET)
        execute_process(COMMAND /usr/bin/xattr -d com.apple.ResourceFork "${_squadspeak_app}" ERROR_QUIET)
        execute_process(COMMAND /usr/bin/codesign --remove-signature "${_squadspeak_app}" ERROR_QUIET)
        execute_process(COMMAND /usr/bin/codesign --force --deep --sign - "${_squadspeak_app}"
            RESULT_VARIABLE _squadspeak_codesign_result)
        if(NOT _squadspeak_codesign_result EQUAL 0)
            message(FATAL_ERROR "Could not ad-hoc sign deployed bundle")
        endif()
    ]])
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(library IN LISTS qt_runtime_libraries)
        get_filename_component(name "${library}" NAME)
        string(REGEX REPLACE "\\.6\\.10\\.2$" ".6" name "${name}")
        install(FILES "${library}" DESTINATION lib RENAME "${name}")
    endforeach()
    # Other Qt, codec and secret-service libraries come from the distribution.
    # Keep the bounded image decoder alongside the application executable.
    install(TARGETS squadspeak squad_image_worker RUNTIME DESTINATION bin)
    install(FILES LICENSE DESTINATION share/squadspeak)
    set_property(TARGET squadspeak PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
    install(DIRECTORY "${squad_apm_prefix}/lib/" DESTINATION lib
        FILES_MATCHING PATTERN "libwebrtc-audio-processing-2.so*")
    install(FILES ui/app.squadspeak.desktop DESTINATION share/applications)
    install(FILES ui/icons/app.png DESTINATION share/icons/hicolor/256x256/apps RENAME app.squadspeak.png)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/third-party/"
        DESTINATION share/squadspeak/THIRD_PARTY_NOTICES
        FILES_MATCHING PATTERN "*.txt" PATTERN "*.spdx")
elseif(WIN32)
    install(TARGETS squadspeak squad_image_worker RUNTIME DESTINATION bin)
    install(FILES LICENSE DESTINATION .)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/third-party/"
        DESTINATION THIRD_PARTY_NOTICES
        FILES_MATCHING PATTERN "*.txt" PATTERN "*.spdx")
endif()

include(CTest)
if(BUILD_TESTING)
    set(ui_test_platform offscreen)
    if(WIN32)
        # The offscreen FreeType database cannot discover Windows TTC fonts.
        # Exercise the same native font fallback as the installed app.
        set(ui_test_platform windows)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test QuickTest)
    add_test(NAME acceptance_handoff COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance_tests.py")
    add_test(NAME translation_sources COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_translation_sources.py"
        $<TARGET_FILE:Qt6::lupdate> "${CMAKE_CURRENT_SOURCE_DIR}")
    set(source_archive_qt_version)
    if(APPLE OR WIN32)
        list(APPEND source_archive_qt_version --qt-version "${Qt6_VERSION}")
    endif()
    add_test(NAME source_archive_manifest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_sources.py" --check ${source_archive_qt_version})
    add_test(NAME source_archive_contract COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_archive_tests.py")
    set_tests_properties(source_archive_manifest source_archive_contract PROPERTIES TIMEOUT 120)
    add_test(NAME dependency_notices COMMAND ${Python3_EXECUTABLE}
        -X warn_default_encoding -W error::EncodingWarning
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_dependency_notices.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/third-party"
        --qt-modules Core Gui Network Qml Quick Multimedia Svg)
    add_test(NAME release_version COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_version_tests.py")
    if(UNIX)
        add_test(NAME release_publication COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_publication_tests.py")
    endif()
    add_test(NAME update_feeds COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_appcasts.py")
    if(TARGET squad_updater)
        add_executable(update_tests MACOSX_BUNDLE tests/update_tests.mm src/app_updates.mm src/app_updates.hpp)
        target_include_directories(update_tests PRIVATE src)
        target_link_libraries(update_tests PRIVATE squad_updater Qt6::Widgets Qt6::Test)
        # This test bundle initializes Sparkle but never requests a feed. Its
        # isolated identity and public key are never used by an app package.
        set(update_test_plist "${CMAKE_CURRENT_BINARY_DIR}/update-tests.plist")
        file(WRITE "${update_test_plist}" "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>
<key>CFBundleIdentifier</key><string>app.squadspeak.update-tests</string>
<key>CFBundleExecutable</key><string>update_tests</string>
<key>CFBundleVersion</key><string>1.0.0</string>
<key>CFBundleName</key><string>SquadSpeak Update Tests</string>
<key>SUPublicEDKey</key><string>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</string>
<key>SUFeedURL</key><string>https://127.0.0.1:9/unused-test-feed.xml</string>
<key>SUEnableAutomaticChecks</key><true/>
<key>SUAutomaticallyUpdate</key><false/>
<key>SUAllowsAutomaticUpdates</key><false/>
<key>SUVerifyUpdateBeforeExtraction</key><true/>
<key>SURequireSignedFeed</key><true/>
</dict></plist>")
        set_target_properties(update_tests PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${update_test_plist}")
        add_test(NAME update_contract COMMAND update_tests)
        if(NOT EXISTS "${sparkle_SOURCE_DIR}/bin/sign_update")
            message(FATAL_ERROR "Pinned Sparkle package is missing its update signing tool")
        endif()
        add_executable(sparkle_replacement_fixture MACOSX_BUNDLE tests/sparkle_replacement_fixture.mm)
        target_link_libraries(sparkle_replacement_fixture PRIVATE squad_updater "-framework Cocoa")
        set_target_properties(sparkle_replacement_fixture PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "app.squadspeak.update-fixture"
            BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks")
        add_test(NAME update_replacement_contract COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sparkle_replacement.py"
            --fixture "$<TARGET_BUNDLE_DIR:sparkle_replacement_fixture>"
            --sparkle "${sparkle_SOURCE_DIR}"
            --sign-update "${sparkle_SOURCE_DIR}/bin/sign_update")
        set_tests_properties(update_replacement_contract PROPERTIES TIMEOUT 180)
    endif()
    if(NOT SQUADSPEAK_STORE_BUILD AND (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        # macOS runs explicitly with Screen Recording permission. Windows and
        # Linux CI capture only their own fixture window without a dialog.
        qt_add_executable(capture_tests MACOSX_BUNDLE tests/capture_tests.cpp)
        target_link_libraries(capture_tests PRIVATE squad_video squad_audio Qt6::Widgets Qt6::Test)
        set_target_properties(capture_tests PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "app.squadspeak.capture-tests")
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(capture_test_plist "${CMAKE_CURRENT_BINARY_DIR}/capture-tests.plist")
            file(WRITE "${capture_test_plist}" "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>capture_tests</string><key>CFBundleIdentifier</key><string>app.squadspeak.capture-tests</string><key>CFBundlePackageType</key><string>APPL</string><key>NSAudioCaptureUsageDescription</key><string>Verify local echo cancellation using two quiet test tones.</string></dict></plist>")
            set_target_properties(capture_tests PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${capture_test_plist}")
        endif()
        if(WIN32)
            add_test(NAME capture_contract COMMAND capture_tests -v2)
            set_tests_properties(capture_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows" TIMEOUT 90)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_program(XVFB_RUN xvfb-run REQUIRED)
            add_test(NAME capture_contract COMMAND "${XVFB_RUN}" -a $<TARGET_FILE:capture_tests> -v2)
            set_tests_properties(capture_contract PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=xcb;WAYLAND_DISPLAY=;XDG_SESSION_TYPE=x11;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp" TIMEOUT 120)
        endif()
    endif()
    qt_add_executable(video_tests tests/video_tests.cpp)
    target_link_libraries(video_tests PRIVATE squad_video Qt6::Test)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(video_tests PRIVATE SQUADSPEAK_DECODE_PROBE=1)
        target_link_options(video_tests PRIVATE "LINKER:--wrap=avcodec_receive_frame")
    endif()
    add_test(NAME video_contract COMMAND video_tests)
    set_tests_properties(video_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform}" TIMEOUT 180)
    qt_add_executable(chat_content_tests tests/chat_content_tests.cpp)
    target_link_libraries(chat_content_tests PRIVATE squad_content squad_session Qt6::Test)
    add_test(NAME chat_content_contract COMMAND chat_content_tests)
    set_tests_properties(chat_content_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform}")
    qt_add_executable(audio_tests tests/audio_tests.cpp)
    target_link_libraries(audio_tests PRIVATE squad_audio Qt6::Test)
    add_test(NAME audio_contract COMMAND audio_tests)
    qt_add_executable(recording_tests tests/recording_tests.cpp src/audio_recording.cpp src/audio_recording.hpp)
    target_include_directories(recording_tests PRIVATE src)
    target_link_libraries(recording_tests PRIVATE Qt6::Multimedia Qt6::Test)
    add_test(NAME recording_contract COMMAND recording_tests)
    qt_add_executable(mixer_tests tests/mixer_tests.cpp)
    target_link_libraries(mixer_tests PRIVATE squad_audio Qt6::Test)
    add_test(NAME mixer_contract COMMAND mixer_tests)
    qt_add_executable(corpus_tests tests/corpus_tests.cpp)
    target_link_libraries(corpus_tests PRIVATE squad_audio Qt6::Test)
    target_compile_definitions(corpus_tests PRIVATE
        SQUAD_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/audio"
        SQUAD_CORPUS_OUTPUT="${CMAKE_BINARY_DIR}/audio-corpus")
    add_test(NAME audio_corpus COMMAND corpus_tests)
    # Instrumented inference is much slower; this is not a performance gate.
    set_tests_properties(audio_corpus PROPERTIES TIMEOUT 600)
    qt_add_executable(radio_tests tests/radio_tests.cpp)
    target_link_libraries(radio_tests PRIVATE squad_radio Qt6::Test)
    add_test(NAME radio_contract COMMAND radio_tests)
    # The reconnect and paced playback cases take 169 s on hosted Linux;
    # allow slower runners to complete the same per-operation assertions.
    set_tests_properties(radio_contract PROPERTIES TIMEOUT 240)
    qt_add_executable(session_tests tests/session_tests.cpp)
    target_link_libraries(session_tests PRIVATE squad_session Qt6::Test)
    target_compile_definitions(session_tests PRIVATE SQUAD_TRANSLATION_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n")
    add_test(NAME session_contract COMMAND session_tests)
    qt_add_executable(license_tests tests/license_tests.cpp src/headless.cpp src/headless.hpp)
    target_link_libraries(license_tests PRIVATE squad_radio Qt6::Test qt6keychain)
    add_test(NAME license_contract COMMAND license_tests)
    set_tests_properties(license_contract PROPERTIES TIMEOUT 60)
    add_executable(license_provider_probe EXCLUDE_FROM_ALL tests/license_provider_probe.cpp)
    target_link_libraries(license_provider_probe PRIVATE squad_session)
    qt_add_executable(ptt_key_tests tests/ptt_key_tests.cpp src/ptt_key.cpp src/ptt_key.hpp)
    target_link_libraries(ptt_key_tests PRIVATE squad_session Qt6::Gui Qt6::Test ${squad_ptt_platform_libraries})
    add_test(NAME ptt_key_contract COMMAND ptt_key_tests)
    qt_add_executable(chat_history_tests tests/chat_history_tests.cpp)
    target_link_libraries(chat_history_tests PRIVATE squad_session squad_content Qt6::Test)
    add_test(NAME chat_history_contract COMMAND chat_history_tests)
    qt_add_executable(media_transport_tests tests/media_transport_tests.cpp)
    target_link_libraries(media_transport_tests PRIVATE squad_session Qt6::Test PkgConfig::OPUS)
    add_test(NAME media_transport_contract COMMAND media_transport_tests)
    qt_add_executable(channel_tests tests/channel_tests.cpp)
    target_link_libraries(channel_tests PRIVATE squad_video squad_audio squad_content Qt6::Test)
    add_test(NAME channel_contract COMMAND channel_tests)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        qt_add_executable(discovery_network_probe tests/discovery_network_probe.cpp)
        target_link_libraries(discovery_network_probe PRIVATE squad_session squad_content)
    endif()
    # More than 1,000 data rows, reconnect waits and 64-client cases share this
    # budget. macOS ARM exhausted 600 seconds with no failed assertion. Qt
    # retains its per-function watchdog; network deadlines remain unchanged.
    set_tests_properties(channel_contract PROPERTIES TIMEOUT 900)
    qt_add_executable(headless_tests tests/headless_tests.cpp src/headless.cpp src/headless.hpp)
    target_link_libraries(headless_tests PRIVATE squad_radio squad_session Qt6::Core Qt6::Test qt6keychain)
    target_compile_definitions(headless_tests PRIVATE SQUAD_HEADLESS_APP="$<TARGET_FILE:squadspeak>"
        SQUAD_TEST_PYTHON="${Python3_EXECUTABLE}")
    add_dependencies(headless_tests squadspeak)
    add_test(NAME headless_contract COMMAND headless_tests)
    # The suite launches dozens of real processes; per-operation deadlines stay
    # in the tests, while this outer budget also allows loaded CI runners.
    set_tests_properties(headless_contract PROPERTIES TIMEOUT 240)
    add_test(NAME desktop_smoke COMMAND squadspeak --smoke-test
        --settings-file "${CMAKE_BINARY_DIR}/smoke/audio.ini"
        --screenshot "${CMAKE_BINARY_DIR}/smoke/audio.png")
    set_tests_properties(desktop_smoke PROPERTIES
        TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
    add_test(NAME recording_smoke COMMAND squadspeak --smoke-test --recording-test
        --settings-file "${CMAKE_BINARY_DIR}/recording-smoke/audio.ini"
        --screenshot "${CMAKE_BINARY_DIR}/recording-smoke/recording.png")
    set_tests_properties(recording_smoke PROPERTIES
        TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
    qt_add_executable(controls_tests tests/controls_tests.cpp src/ptt_key.cpp src/ptt_key.hpp src/audio_model.cpp src/audio_model.hpp src/audio_recording.cpp src/audio_recording.hpp)
    if(QT6_IS_SHARED_LIBS_BUILD)
        # Tests load QML from source and do not deploy plugins. Scanning the
        # repository root also traverses local build/dependency directories.
        set_property(TARGET controls_tests PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    endif()
    target_link_libraries(controls_tests PRIVATE squad_video squad_radio squad_audio squad_session squad_content Qt6::QuickTest Qt6::QuickControls2 Qt6::Multimedia ${squad_ptt_platform_libraries})
    add_test(NAME audio_controls COMMAND controls_tests -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_controls.qml")
    set_tests_properties(audio_controls PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
    add_test(NAME frequency_display COMMAND controls_tests -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_spectrum.qml")
    set_tests_properties(frequency_display PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke")
    add_test(NAME channel_controls COMMAND controls_tests -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml")
    set_tests_properties(channel_controls PROPERTIES
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke")
    foreach(scale 1.5 2)
        add_test(NAME avatar_scale_${scale} COMMAND controls_tests
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml"
            DirectChannelControls::test_grayscalePreservesPortraitGeometry
            DirectChannelControls::test_freeAvatarChoicesCannotGrantSupporterAndAboutShowsPurchaseAvailability
            DirectChannelControls::test_avatarUnloadsArtworkWhenHiddenAndReloadsAfterStateChanges)
        set_tests_properties(avatar_scale_${scale} PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=${scale};SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke/avatars-${scale}")
    endforeach()
    add_dependencies(channel_tests squad_image_worker)
    add_dependencies(controls_tests squad_image_worker)
    add_dependencies(chat_content_tests squad_image_worker)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_property(TEST channel_controls APPEND PROPERTY ENVIRONMENT
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
        pkg_check_modules(X11_TEST REQUIRED IMPORTED_TARGET x11 xtst)
        find_program(XVFB_RUN xvfb-run REQUIRED)
        qt_add_executable(linux_ptt_tests tests/linux_ptt_tests.cpp src/ptt_key.cpp src/ptt_key.hpp)
        target_link_libraries(linux_ptt_tests PRIVATE squad_session Qt6::Gui Qt6::Test PkgConfig::X11_TEST ${squad_ptt_platform_libraries})
        add_test(NAME linux_global_ptt COMMAND "${XVFB_RUN}" -a $<TARGET_FILE:linux_ptt_tests>)
        set_tests_properties(linux_global_ptt PROPERTIES TIMEOUT 30
            ENVIRONMENT "QT_QPA_PLATFORM=xcb;WAYLAND_DISPLAY=;XDG_SESSION_TYPE=x11")
    endif()
    # VideoOutput needs an RHI renderer; the basic software scene graph cannot
    # prove displayed pixels. Use the production path without capture hardware.
    set(render_test_launcher)
    set(screen_test_environment "QT_QUICK_BACKEND=;QT_QUICK_CONTROLS_STYLE=Basic;SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(render_test_launcher "${XVFB_RUN}" -a)
        list(APPEND screen_test_environment "QT_QPA_PLATFORM=xcb" "QSG_RHI_BACKEND=opengl" "LIBGL_ALWAYS_SOFTWARE=1"
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
    elseif(APPLE)
        list(APPEND screen_test_environment "QT_QPA_PLATFORM=cocoa" "QSG_RHI_BACKEND=metal")
    elseif(WIN32)
        list(APPEND screen_test_environment "QT_QPA_PLATFORM=windows" "QSG_RHI_BACKEND=d3d11" "QSG_RHI_PREFER_SOFTWARE_RENDERER=1")
    endif()
    add_test(NAME screen_rendering COMMAND ${render_test_launcher} $<TARGET_FILE:controls_tests>
        -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_screen.qml")
    add_test(NAME desktop_rendering_smoke COMMAND ${render_test_launcher} $<TARGET_FILE:squadspeak>
        --smoke-test --settings-file "${CMAKE_BINARY_DIR}/render-smoke/audio.ini"
        --screenshot "${CMAKE_BINARY_DIR}/smoke/graphics-audio.png")
    set_tests_properties(screen_rendering desktop_rendering_smoke PROPERTIES TIMEOUT 60 ENVIRONMENT "${screen_test_environment}")
    if(APPLE)
        set_tests_properties(desktop_smoke recording_smoke desktop_rendering_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Could not find permission plugin for QMicrophonePermission")
    endif()
    # Keep timing/load measurements and native loopback capture undisturbed.
    set_tests_properties(channel_contract video_contract PROPERTIES RUN_SERIAL TRUE)
    if(TEST capture_contract)
        set_tests_properties(capture_contract PROPERTIES RUN_SERIAL TRUE)
    endif()
    # GUI, clipboard and audio-device fixtures share the runner's desktop.
    set_tests_properties(radio_contract headless_contract desktop_smoke recording_smoke
        audio_controls frequency_display channel_controls avatar_scale_1.5 avatar_scale_2
        screen_rendering desktop_rendering_smoke PROPERTIES RESOURCE_LOCK desktop)
    if(TEST linux_global_ptt)
        set_tests_properties(linux_global_ptt PROPERTIES RESOURCE_LOCK desktop)
    endif()
endif()
