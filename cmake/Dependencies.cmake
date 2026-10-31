# Native dependency versions, patches and compiler settings form one cache input.
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
if(ANDROID)
    add_link_options(-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
endif()

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Quick QuickControls2 Multimedia Network Sql LinguistTools)
if(ANDROID AND NOT Qt6_VERSION VERSION_EQUAL "6.11.3")
    message(FATAL_ERROR "The Android FFmpeg headers require the verified Qt 6.11.3 runtime.")
endif()
if(SQUADSPEAK_IOS)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Concurrent)
endif()
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
elseif(SQUADSPEAK_IOS OR ANDROID)
    foreach(dependency IN ITEMS OPUS SAMPLERATE)
        string(TOLOWER "${dependency}" dependency_lower)
        find_path(squad_${dependency_lower}_include_dir
            ${dependency_lower}.h PATHS "${SQUADSPEAK_MOBILE_DEPS_ROOT}/include" PATH_SUFFIXES opus NO_DEFAULT_PATH REQUIRED)
        find_library(squad_${dependency_lower}_library
            NAMES ${dependency_lower} PATHS "${SQUADSPEAK_MOBILE_DEPS_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
        add_library(PkgConfig::${dependency} INTERFACE IMPORTED)
        set_target_properties(PkgConfig::${dependency} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${squad_${dependency_lower}_include_dir}"
            INTERFACE_LINK_LIBRARIES "${squad_${dependency_lower}_library}")
    endforeach()
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

# Rebuild only the affected modules against the exact installed Qt ABI.
# Shared SDK libraries remain the owners of all other Qt functionality.
set(qt_runtime_modules)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT Qt6_VERSION VERSION_EQUAL "6.10.2")
        message(FATAL_ERROR "The verified Linux runtime requires Qt 6.10.2 with the bundled fixes.")
    endif()
    FetchContent_Declare(qt_capture
        URL https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtmultimedia-everywhere-src-6.10.2.tar.xz
        URL_HASH SHA256=93f7ef0106fbd731165a2723f3e436c911fc5e6880f5bc987b55516c20833e2b
        SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    list(APPEND qt_runtime_modules capture)
    # Includes upstream PipeWire listener lifetime fixes 1c6fb56 and 5a0d4a1:
    # unregister under the event-loop lock and consume callbacks before invoking.
    set(qt_capture_patches qt-wayland.patch)
    FetchContent_Declare(qt_network
        URL https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtbase-everywhere-src-6.10.2.tar.xz
        URL_HASH SHA256=aeb78d29291a2b5fd53cb55950f8f5065b4978c25fb1d77f627d695ab9adf21e
        SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(qt_network_patches qt-network-security.patch)
    list(APPEND qt_runtime_modules network)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux|Windows)$")
    if(Qt6_VERSION VERSION_EQUAL "6.10.2")
        FetchContent_Declare(qt_qml
            URL https://download.qt.io/archive/qt/6.10/6.10.2/submodules/qtdeclarative-everywhere-src-6.10.2.tar.xz
            URL_HASH SHA256=a249914ff66cdcdbf0df8b5ffad997a2ee6dce01cc17d43c6cc56fdc1d0f4b0f
            SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(qt_qml_patches qt-qml-gc.patch qt-qml-runtime.patch)
        list(APPEND qt_runtime_modules qml)
    elseif(Qt6_VERSION VERSION_EQUAL "6.11.3")
        FetchContent_Declare(qt_qml
            URL https://master.qt.io/online/qtsdkrepository/all_os/qt/qt6_6113_unix_line_endings_src/qt.qt6.6113.src/6.11.3-0-202610220312qtdeclarative-everywhere-src-6.11.3.tar.xz
            URL_HASH SHA256=90e941624ac1e99c376f9c32ef7a580519915d8d42adf4ad160142918f935ae2
            SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(qt_qml_patches qt-qml-runtime.patch)
        list(APPEND qt_runtime_modules qml)
    else()
        message(FATAL_ERROR "The verified Qt Qml runtime requires Qt 6.10.2 or 6.11.3.")
    endif()
endif()
if(qt_runtime_modules)
    find_package(Git REQUIRED)
    get_target_property(qt_qmake Qt6::qmake LOCATION)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_PREFIX
        OUTPUT_VARIABLE qt_prefix OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_LIBS
        OUTPUT_VARIABLE qt_libs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(RELATIVE_PATH qt_runtime_libdir "${qt_prefix}" "${qt_libs}")
    foreach(module IN LISTS qt_runtime_modules)
        FetchContent_MakeAvailable(qt_${module})
        if(module STREQUAL "capture")
            set(component Multimedia)
        elseif(module STREQUAL "network")
            set(component Network)
        else()
            set(component Qml)
        endif()
        set(source "${qt_${module}_SOURCE_DIR}")
        foreach(patch_name IN LISTS qt_${module}_patches)
            set(patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${patch_name}")
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
                WORKING_DIRECTORY "${source}" RESULT_VARIABLE unpatched OUTPUT_QUIET ERROR_QUIET)
            if(unpatched EQUAL 0)
                execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
                    WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
            else()
                execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
                    WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
            endif()
        endforeach()
        set(module_cmake_args)
        if(module STREQUAL "network")
            # Build only Network against the installed Core/CorePrivate. A full
            # QtBase build would produce another Core beside Network and put it
            # on the application's runtime path.
            # Preserve every exported Network feature. Missing development
            # dependencies must fail configuration instead of removing proxy,
            # authentication, compression or socket functionality silently.
            foreach(visibility IN ITEMS PUBLIC PRIVATE)
                foreach(state IN ITEMS ENABLED DISABLED)
                    get_target_property(features Qt6::Network QT_${state}_${visibility}_FEATURES)
                    if(features)
                        foreach(feature IN LISTS features)
                            if(state STREQUAL "ENABLED")
                                list(APPEND module_cmake_args "-DFEATURE_${feature}=ON")
                            else()
                                list(APPEND module_cmake_args "-DFEATURE_${feature}=OFF")
                            endif()
                        endforeach()
                    endif()
                endforeach()
            endforeach()
            list(APPEND module_cmake_args -DSQUADSPEAK_NETWORK_ONLY=ON -DCMAKE_SKIP_RPATH=ON)
        endif()
        set(module_build "${CMAKE_CURRENT_BINARY_DIR}/qt-${module}")
        set(libdir "${module_build}/${qt_runtime_libdir}")
        set(module_byproducts)
        if(APPLE)
            set(library "${libdir}/Qt${component}.framework/Versions/A/Qt${component}")
        elseif(WIN32)
            set(libdir "${module_build}/bin")
            set(library "${libdir}/Qt6${component}.dll")
            set(import_library "${module_build}/lib/Qt6${component}.lib")
            list(APPEND module_byproducts "${import_library}")
        else()
            set(library "${libdir}/libQt6${component}.so.${Qt6_VERSION}")
            list(APPEND qt_runtime_libraries "${library}")
        endif()
        if(module STREQUAL "qml")
            set(qt_qml_library "${library}")
        endif()
        set(module_build_options
            BINARY_DIR "${module_build}"
            CMAKE_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF
                "-DQt6_DIR=${Qt6_DIR}" -DFEATURE_gstreamer=OFF ${module_cmake_args}
                "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}"
                "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
                "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}" "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
                "-DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
                "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
                "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target ${component} --parallel 4
            USES_TERMINAL_BUILD TRUE
            BUILD_BYPRODUCTS "${library}" ${module_byproducts})
        if(module STREQUAL "capture")
            ExternalProject_Add(squad_qt_capture_build
                SOURCE_DIR "${qt_capture_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
                INSTALL_COMMAND "" ${module_build_options})
        elseif(module STREQUAL "network")
            ExternalProject_Add(squad_qt_network_build
                SOURCE_DIR "${qt_network_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
                INSTALL_COMMAND "" ${module_build_options})
        else()
            ExternalProject_Add(squad_qt_qml_build
                SOURCE_DIR "${qt_qml_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
                INSTALL_COMMAND "" ${module_build_options})
        endif()
        get_target_property(configurations Qt6::${component} IMPORTED_CONFIGURATIONS)
        foreach(configuration IN LISTS configurations)
            set_property(TARGET Qt6::${component} PROPERTY "IMPORTED_LOCATION_${configuration}" "${library}")
            if(WIN32)
                set_property(TARGET Qt6::${component} PROPERTY "IMPORTED_IMPLIB_${configuration}" "${import_library}")
            endif()
        endforeach()
        set_property(TARGET Qt6::${component} PROPERTY IMPORTED_LOCATION "${library}")
        if(WIN32)
            set_property(TARGET Qt6::${component} PROPERTY IMPORTED_IMPLIB "${import_library}")
        endif()
        add_dependencies(Qt6::${component} squad_qt_${module}_build)
        list(PREPEND CMAKE_BUILD_RPATH "${libdir}")
    endforeach()
endif()

# Qt Multimedia and the app must resolve one FFmpeg ABI. macOS builds the
# matching version with VideoToolbox lifetime fixes; Windows uses its
# staged Qt SDK and Linux uses the distribution packages.
add_library(squad_ffmpeg INTERFACE)
if(APPLE AND NOT SQUADSPEAK_IOS)
    FetchContent_Declare(ffmpeg
        URL https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.5.tar.gz
        URL_HASH SHA256=e3963a50831c985933e1a625ed566ec4c7adb5c012c34fa9f84438e1d61bdacc
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
elseif(SQUADSPEAK_IOS OR ANDROID)
    find_path(squad_ffmpeg_include_dir libavcodec/avcodec.h
        PATHS "${SQUADSPEAK_MOBILE_DEPS_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    target_include_directories(squad_ffmpeg INTERFACE "${squad_ffmpeg_include_dir}")
    foreach(library IN ITEMS avcodec avutil swscale avformat swresample)
        set(squad_codec_library_dir "${SQUADSPEAK_MOBILE_DEPS_ROOT}/lib")
        if(ANDROID)
            # Use the same FFmpeg runtime as Qt Multimedia, as on Windows.
            set(squad_codec_library_dir "${QT6_INSTALL_PREFIX}/lib")
        endif()
        find_library(squad_ffmpeg_${library} ${library}
            PATHS "${squad_codec_library_dir}" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(squad_ffmpeg INTERFACE "${squad_ffmpeg_${library}}")
        if(ANDROID)
            list(APPEND squad_android_libraries "${squad_ffmpeg_${library}}")
        endif()
    endforeach()
    if(SQUADSPEAK_IOS)
        target_link_libraries(squad_ffmpeg INTERFACE
            "-framework AVFoundation"
            "-framework CoreFoundation"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework Security"
            "-framework VideoToolbox")
    else()
        list(APPEND squad_android_libraries "${OPENSSL_CRYPTO_LIBRARY}" "${OPENSSL_SSL_LIBRARY}")
    endif()
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

# Upstream inference and its matching sparse model. Keep both pins together.
# The smaller model uses the same API and needs no runtime downloads.
FetchContent_Declare(rnnoise
    URL https://github.com/xiph/rnnoise/archive/70f1d256acd4b34a572f999a05c87bf00b67730d.tar.gz
    URL_HASH SHA256=f61ee0b3f4c4cd337303e003d333357c5eaf25ef5d75a742109ee59e9a0a3932
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(rnnoise_model
    URL https://media.xiph.org/rnnoise/models/rnnoise_data-0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37.tar.gz
    URL_HASH SHA256=0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(rnnoise rnnoise_model)
add_library(squad_rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c ${rnnoise_model_SOURCE_DIR}/src/rnnoise_data_little.c)
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
