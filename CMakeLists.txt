cmake_minimum_required(VERSION 3.24)
# Match the desktop Qt baseline instead of inheriting the build machine's OS.
# Explicit deployment targets and mobile toolchains keep their own settings.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET
        AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
include(cmake/Version.cmake)
project(SquadSpeak VERSION ${SQUADSPEAK_NUMERIC_VERSION} LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(SQUADSPEAK_IOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(SQUADSPEAK_IOS ON)
    function(squad_ios_simulator_identity target)
        if(NOT CMAKE_OSX_SYSROOT MATCHES "[iI][pP]hone[sS]imulator" OR CMAKE_GENERATOR STREQUAL "Xcode")
            return()
        endif()
        # Xcode embeds simulator entitlements in the executable. Ninja must do
        # the same so the simulated keychain can identify each application.
        set(entitlements "${CMAKE_CURRENT_BINARY_DIR}/${target}-simulator.entitlements")
        file(GENERATE OUTPUT "${entitlements}"
            CONTENT "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>
<key>application-identifier</key><string>SIMULATOR.$<TARGET_PROPERTY:${target},MACOSX_BUNDLE_GUI_IDENTIFIER></string>
</dict></plist>\n")
        target_link_options(${target} PRIVATE "LINKER:-sectcreate,__TEXT,__entitlements,${entitlements}")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${entitlements}")
    endfunction()
endif()
if(SQUADSPEAK_IOS OR ANDROID)
    set(SQUADSPEAK_MOBILE_DEPS_ROOT "" CACHE PATH
        "Cross-compiled OpenSSL, Opus, libsamplerate and FFmpeg prefix")
    if(NOT IS_DIRECTORY "${SQUADSPEAK_MOBILE_DEPS_ROOT}")
        message(FATAL_ERROR
            "Mobile builds require -DSQUADSPEAK_MOBILE_DEPS_ROOT=<target dependency prefix>. "
            "Do not use host libraries for a mobile build.")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${SQUADSPEAK_MOBILE_DEPS_ROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${SQUADSPEAK_MOBILE_DEPS_ROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
    set(OPENSSL_ROOT_DIR "${SQUADSPEAK_MOBILE_DEPS_ROOT}" CACHE PATH
        "Target OpenSSL prefix" FORCE)
    set(OPENSSL_USE_STATIC_LIBS ${SQUADSPEAK_IOS})
    if(ANDROID)
        set(QT_USE_TARGET_ANDROID_BUILD_DIR ON CACHE BOOL "Separate Android packages for app and tests" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto_3.so" CACHE FILEPATH "Android OpenSSL crypto" FORCE)
        set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl_3.so" CACHE FILEPATH "Android OpenSSL TLS" FORCE)
    endif()
endif()

include(cmake/Dependencies.cmake)

add_library(squad_audio src/audio_processor.cpp src/audio_profiles.cpp src/voice_mixer.cpp src/echo_canceller.cpp src/output_monitor.cpp)
if(SQUADSPEAK_IOS)
    target_sources(squad_audio PRIVATE src/ios_audio_session.mm)
    set_source_files_properties(src/ios_audio_session.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()
if(APPLE)
    target_sources(squad_audio PRIVATE src/speech_output_apple.mm)
    set_source_files_properties(src/speech_output_apple.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(squad_audio PRIVATE "-framework AVFoundation")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(squad_audio PRIVATE src/output_monitor_mac.mm)
    set_source_files_properties(src/output_monitor_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(squad_audio PRIVATE "-framework CoreAudio" "-framework Foundation" "-framework AVFoundation")
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

include(cmake/TeamSpeak.cmake)

add_library(squad_session src/local_channel_teamspeak.cpp src/voice_session.cpp src/voice_session.hpp src/tls_identity.cpp src/tls_identity.hpp src/local_channel.cpp src/local_channel_chat.cpp src/local_channel_private.cpp src/local_channel_control.cpp src/local_channel_discovery.cpp src/local_channel_video.cpp src/local_channel.hpp src/media_transport.cpp src/media_transport.hpp src/chat_history.cpp src/chat_history.hpp src/license.cpp src/license.hpp)
target_include_directories(squad_session PUBLIC src)
# Version changes only rebuild entrypoints that report the version.
set_source_files_properties(src/headless.cpp tests/headless_tests.cpp PROPERTIES
    COMPILE_DEFINITIONS "SQUADSPEAK_VERSION=\"${SQUADSPEAK_VERSION}\"")
option(SQUADSPEAK_STORE_BUILD "Build a free-only store distribution" OFF)
set(SQUADSPEAK_GITHUB_CLIENT_ID "Ov23li8PQoxYilNhKyrn" CACHE STRING "Public Yuna Supporter OAuth client ID")
if(SQUADSPEAK_GITHUB_CLIENT_ID AND NOT SQUADSPEAK_GITHUB_CLIENT_ID MATCHES "^[A-Za-z0-9_]+$")
    message(FATAL_ERROR "Invalid public GitHub OAuth client ID")
endif()
target_compile_definitions(squad_session PUBLIC SQUADSPEAK_STORE_BUILD=$<BOOL:${SQUADSPEAK_STORE_BUILD}>
    SQUADSPEAK_GITHUB_CLIENT_ID="${SQUADSPEAK_GITHUB_CLIENT_ID}")
target_link_libraries(squad_session PUBLIC Qt6::Core Qt6::Network Qt6::Sql PRIVATE OpenSSL::Crypto qt6keychain PkgConfig::OPUS datachannel-static squad_teamspeak)
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

add_library(squad_language src/local_language.cpp src/local_language.hpp)
target_link_libraries(squad_language PUBLIC squad_audio Qt6::Gui)
target_include_directories(squad_language PUBLIC src)
if(APPLE)
    set(squad_apple_sdk macosx)
    set(squad_swift_os macos)
    set(squad_swift_environment "")
    if(SQUADSPEAK_IOS)
        set(squad_apple_sdk iphoneos)
        set(squad_swift_os ios)
        if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            set(squad_apple_sdk iphonesimulator)
            set(squad_swift_environment "-simulator")
        endif()
    endif()
    execute_process(COMMAND xcrun --sdk ${squad_apple_sdk} --show-sdk-version
        OUTPUT_VARIABLE squad_apple_sdk_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(squad_apple_sdk_version VERSION_LESS 26)
        message(FATAL_ERROR "Local Apple language processing requires Xcode 26 or newer. The app keeps its platform deployment targets.")
    endif()
    set(squad_swift_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_OSX_ARCHITECTURES)
        set(squad_swift_arch "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    set(CMAKE_Swift_COMPILER_TARGET "${squad_swift_arch}-apple-${squad_swift_os}${CMAKE_OSX_DEPLOYMENT_TARGET}${squad_swift_environment}")
    enable_language(Swift)
    execute_process(COMMAND xcrun --sdk ${squad_apple_sdk} --show-sdk-path
        OUTPUT_VARIABLE squad_apple_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    target_link_directories(squad_language INTERFACE "${squad_apple_sysroot}/usr/lib/swift")
    target_sources(squad_language PRIVATE src/local_language_native.h src/local_language_apple.swift)
    target_compile_definitions(squad_language PRIVATE $<$<COMPILE_LANGUAGE:CXX>:SQUAD_LOCAL_LANGUAGE_NATIVE>)
    if(squad_apple_sdk_version VERSION_GREATER_EQUAL 26.4)
        target_compile_options(squad_language PRIVATE $<$<COMPILE_LANGUAGE:Swift>:-DSQUAD_TRANSLATION_STRATEGY>)
    endif()
    target_link_options(squad_language INTERFACE "LINKER:-weak_framework,Translation" "LINKER:-weak_framework,Speech")
    target_link_libraries(squad_language PRIVATE
        "-framework NaturalLanguage"
        "-framework AVFAudio"
        "-framework SwiftUI")
    if(SQUADSPEAK_IOS)
        target_link_libraries(squad_language PRIVATE "-framework UIKit")
    else()
        target_link_libraries(squad_language PRIVATE "-framework AppKit")
    endif()
endif()

add_library(squad_content src/chat_content.cpp src/chat_content.hpp)
target_include_directories(squad_content PUBLIC src)
target_link_libraries(squad_content PUBLIC Qt6::Gui Qt6::Network)
if(SQUADSPEAK_IOS)
    target_link_libraries(squad_content PRIVATE Qt6::Concurrent)
endif()
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

if(NOT SQUADSPEAK_IOS)
    add_executable(squad_image_worker src/image_worker.cpp)
    target_link_libraries(squad_image_worker PRIVATE squad_content Qt6::Core)
endif()

# Keep production QML scanning inside the UI tree, away from tests and build outputs.
include(cmake/Updates.cmake)
if(ANDROID)
    set(squad_android_package "${CMAKE_BINARY_DIR}/android-package")
    file(MAKE_DIRECTORY "${squad_android_package}/res/drawable")
    configure_file(ui/icons/app.png "${squad_android_package}/res/drawable/icon.png" COPYONLY)
    file(READ "${QT6_INSTALL_PREFIX}/src/android/templates/AndroidManifest.xml" squad_android_manifest)
    string(REPLACE "org.qtproject.qt.android.bindings.QtActivity" "app.squadspeak.Activity"
        squad_android_manifest "${squad_android_manifest}")
    string(REPLACE "android:launchMode=\"singleTop\"" "android:launchMode=\"singleTop\" android:windowSoftInputMode=\"adjustResize\""
        squad_android_manifest "${squad_android_manifest}")
    file(CONFIGURE OUTPUT "${squad_android_package}/AndroidManifest.xml" CONTENT "${squad_android_manifest}" @ONLY)
    configure_file(ui/android/src/app/squadspeak/Activity.java
        "${squad_android_package}/src/app/squadspeak/Activity.java" COPYONLY)
endif()
add_subdirectory(ui)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
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
    if(qt_qml_library)
        # macdeployqt can resolve the SDK copy while walking plugin dependencies.
        install(CODE "file(COPY_FILE \"${qt_qml_library}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/squadspeak.app/Contents/Frameworks/QtQml.framework/Versions/A/QtQml\" ONLY_IF_DIFFERENT)")
    endif()
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
    qt_add_executable(video_tests tests/video_tests.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_library(capture_permission_probe SHARED tests/capture_permission_probe.cpp)
        target_link_libraries(capture_permission_probe PRIVATE "-framework CoreGraphics")
        target_link_libraries(video_tests PRIVATE capture_permission_probe)
    endif()
    target_link_libraries(video_tests PRIVATE squad_video Qt6::Test)
    if(NOT SQUADSPEAK_IOS AND NOT ANDROID)
        qt_add_executable(local_language_tests tests/local_language_tests.cpp src/local_language.cpp src/local_language.hpp)
        target_compile_definitions(local_language_tests PRIVATE SQUAD_LOCAL_LANGUAGE_NATIVE)
        target_link_libraries(local_language_tests PRIVATE squad_audio Qt6::Gui Qt6::Test)
        add_test(NAME local_language_contract COMMAND local_language_tests -platform ${ui_test_platform})
        set_tests_properties(local_language_contract PROPERTIES TIMEOUT 30)
    endif()
    if(APPLE)
        qt_add_executable(native_language_tests MACOSX_BUNDLE tests/local_language_tests.cpp)
        target_compile_definitions(native_language_tests PRIVATE SQUAD_NATIVE_LANGUAGE_ACCEPTANCE)
        target_link_libraries(native_language_tests PRIVATE squad_language Qt6::Test)
        set_target_properties(native_language_tests PROPERTIES LINKER_LANGUAGE CXX
            MACOSX_BUNDLE_GUI_IDENTIFIER "app.squadspeak.language-tests"
            MACOSX_BUNDLE_BUNDLE_NAME "SquadSpeak Language Tests")
        if(NOT SQUADSPEAK_IOS)
            add_test(NAME native_language_contract COMMAND native_language_tests
                nativeCapabilitiesAndSameLanguageTranslation nativePendingCallbacksDoNotOutliveTheirOwner)
            set_tests_properties(native_language_contract PROPERTIES TIMEOUT 45 LABELS "native")
        endif()
    endif()
    qt_add_executable(audio_tests tests/audio_tests.cpp)
    target_link_libraries(audio_tests PRIVATE squad_audio Qt6::Test)
    if(APPLE)
        target_link_libraries(audio_tests PRIVATE Qt6::Gui)
    endif()
    qt_add_executable(mixer_tests tests/mixer_tests.cpp)
    target_link_libraries(mixer_tests PRIVATE squad_audio Qt6::Test)
    qt_add_resources(mixer_tests "speech_fixture" PREFIX "/audio" BASE tests/audio FILES tests/audio/speech1.wav)
    qt_add_executable(media_transport_tests tests/media_transport_tests.cpp)
    target_link_libraries(media_transport_tests PRIVATE squad_session Qt6::Test PkgConfig::OPUS datachannel-static plog::plog)
    qt_add_executable(channel_tests tests/channel_tests.cpp)
    target_link_libraries(channel_tests PRIVATE squad_video squad_audio squad_content Qt6::Test)
    add_subdirectory(tests)
    if(APPLE AND NOT SQUADSPEAK_IOS)
        add_test(NAME native_language_controls COMMAND controls_tests
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml"
            DirectChannelControls::test_channelLanguageOptionsAreExplicitAndLocal
            DirectChannelControls::test_missingLanguagesAskInAppAndWaitForDownload
            DirectChannelControls::test_incomingChannelTranslationKeepsOriginalAndHostHistory
            DirectChannelControls::test_outgoingTranslationPreviewCannotSendAnOlderDraft)
        set_tests_properties(native_language_controls PROPERTIES TIMEOUT 90 LABELS "native" RESOURCE_LOCK desktop
            ENVIRONMENT "QT_QPA_PLATFORM=cocoa;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
    endif()
    if(SQUADSPEAK_IOS)
        qt_add_executable(chat_content_tests MACOSX_BUNDLE tests/chat_content_tests.cpp)
        target_link_libraries(channel_tests PRIVATE qt6keychain)
        target_link_libraries(chat_content_tests PRIVATE squad_content squad_session Qt6::Test Qt6::Concurrent)
        set(SQUADSPEAK_IOS_SIMULATOR_DEVICE_ID "booted" CACHE STRING
            "Simulator device used by the iOS contracts")
        function(squad_add_ios_test target test_name)
            string(REGEX REPLACE "_tests$" "" fixture "${target}")
            string(REPLACE "_" "-" bundle_name "${fixture}")
            target_link_libraries(${target} PRIVATE Qt6::Gui)
            set_target_properties(${target} PROPERTIES MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_GUI_IDENTIFIER "app.squadspeak.${bundle_name}-tests"
                MACOSX_BUNDLE_BUNDLE_NAME "SquadSpeak Tests"
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/ui/Info.plist.in"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
            if(NOT TEST ${fixture}_install)
                squad_ios_simulator_identity(${target})
            endif()
            if(NOT target STREQUAL "controls_tests")
                set_property(TARGET ${target} PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
            endif()
            if(NOT TEST ${fixture}_install)
                add_test(NAME ${fixture}_install COMMAND xcrun simctl install
                    "${SQUADSPEAK_IOS_SIMULATOR_DEVICE_ID}" "$<TARGET_BUNDLE_DIR:${target}>")
                set_tests_properties(${fixture}_install PROPERTIES
                    TIMEOUT 30 FIXTURES_SETUP ios_${fixture} RESOURCE_LOCK ios_simulator)
            endif()
            add_test(NAME ${test_name} COMMAND xcrun simctl launch --console-pty
                "--arch=${CMAKE_OSX_ARCHITECTURES}" "${SQUADSPEAK_IOS_SIMULATOR_DEVICE_ID}"
                "app.squadspeak.${bundle_name}-tests" ${ARGN})
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120
                FIXTURES_REQUIRED ios_${fixture} RESOURCE_LOCK ios_simulator
                PASS_REGULAR_EXPRESSION "Totals: [1-9][0-9]* passed, 0 failed"
                FAIL_REGULAR_EXPRESSION "FAIL!|QFATAL")
        endfunction()
        squad_add_ios_test(chat_content_tests chat_content_contract)
        squad_add_ios_test(native_language_tests native_language_contract
            nativeCapabilitiesAndSameLanguageTranslation nativePendingCallbacksDoNotOutliveTheirOwner)
        squad_add_ios_test(audio_tests speech_contract
            speechVoiceSelectionUsesLanguageQualityAndStableTies speechProducesLocalPcmAndStopsWithoutLatePlayback)
        squad_add_ios_test(controls_tests apple_controls
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml"
            DirectChannelControls::test_botVoiceCanBeSelectedAndReturnedToAutomatic
            DirectChannelControls::test_botVoiceDownloadSettingsAndOptIn
            DirectChannelControls::test_platformSpecificLanguageControls
            DirectChannelControls::test_channelLanguageOptionsAreExplicitAndLocal
            DirectChannelControls::test_missingLanguagesAskInAppAndWaitForDownload
            DirectChannelControls::test_incomingChannelTranslationKeepsOriginalAndHostHistory
            DirectChannelControls::test_dockPreferenceIsMacOnlyAndKeepsVoiceState)
        squad_add_ios_test(controls_tests ios_audio_capture
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml"
            DirectChannelControls::test_iosAudioSessionCapturesAndCanRestart
            DirectChannelControls::test_iosAudioSessionTransmitsWithPlaybackAndMute)
        set_tests_properties(ios_audio_capture PROPERTIES
            ENVIRONMENT "SIMCTL_CHILD_SQUADSPEAK_TEST_INPUT=present")
        squad_add_ios_test(controls_tests channel_controls
            -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml"
            DirectChannelControls::test_chatSendsOnlyAfterHostReceiptAndKeepsVoiceAvailable:English
            DirectChannelControls::test_all64MembersAreReachableFromChannelSummary
            DirectChannelControls::test_returningToChatPaintsWithoutScrolling
            DirectChannelControls::test_compactChatSendsPreparedImage
            DirectChannelControls::test_settingsContainProfileAndAudioInTheChannelWindow
            DirectChannelControls::test_scrollLoadsOlderMessagesWithoutDownloadingThemAtOpen
            DirectChannelControls::test_scrollbarTracksLoadedMessagesWithoutResizing
            DirectChannelControls::test_chatKeepsReadingPositionAcrossResize
            DirectChannelControls::test_wheelScrollsInsideOversizedMessage
            DirectChannelControls::test_touchScrollsChatAndReleasesPtt)
        set_tests_properties(channel_controls PROPERTIES
            TIMEOUT 180
            ENVIRONMENT "SIMCTL_CHILD_SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke")
        squad_add_ios_test(channel_tests channel_contract
            tlsWithoutDeviceCertificateCannotReachAdmission
            headlessAutomaticallyAdmitsUnapprovedClientsExceptBans
            ownerCanKickBanAndUnblockWithoutJoining
            passwordsPreserveActiveMembersAndPersistPerHostSecrets
            remotePttNeedsSeparateApprovalAndCombinesControllers
            chatSurvivesHostRestartAndReplaysToNewApprovedParticipants)
        squad_add_ios_test(channel_tests native_identity_contract simulatorIdentitySurvivesReload)
        squad_add_ios_test(mixer_tests mixer_contract)
        squad_add_ios_test(media_transport_tests media_transport_contract)
        squad_add_ios_test(video_tests video_contract
            roundTripAndQualityChanges nativePixelFormatsKeepColorsAcrossChanges
            invalidFramesAndFormatsFailBoundedly udpScreenRoundTripDecodesRealFrames)
        return()
    endif()
    if(ANDROID)
        find_program(SQUAD_ANDROID_TEST_RUNNER androidtestrunner
            HINTS "${QT_HOST_PATH}/bin" REQUIRED)
        find_program(SQUAD_ADB adb HINTS "${ANDROID_SDK_ROOT}/platform-tools" REQUIRED)
        function(squad_add_android_test target test_name)
            target_link_libraries(${target} PRIVATE Qt6::Gui)
            string(REPLACE "_" "" package_name "${target}")
            set_target_properties(${target} PROPERTIES
                QT_ANDROID_PACKAGE_NAME "app.squadspeak.${package_name}"
                QT_ANDROID_APP_NAME "SquadSpeak Tests"
                QT_ANDROID_PACKAGE_SOURCE_DIR "${squad_android_package}"
                QT_ANDROID_TARGET_SDK_VERSION 36
                QT_ANDROID_EXTRA_LIBS "${squad_android_libraries}")
            if(target STREQUAL "media_transport_tests" OR target STREQUAL "mixer_tests")
                set_property(TARGET ${target} PROPERTY QT_ANDROID_EXTRA_LIBS
                    "${OPENSSL_CRYPTO_LIBRARY};${OPENSSL_SSL_LIBRARY}")
            endif()
            if(NOT target STREQUAL "controls_tests")
                set_property(TARGET ${target} PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
            endif()
            get_target_property(output ${target} BINARY_DIR)
            set(package "${output}/android-build-${target}")
            add_test(NAME ${test_name} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_android_test.py"
                --adb "${SQUAD_ADB}" --package "app.squadspeak.${package_name}"
                --log "${CMAKE_BINARY_DIR}/android-logs/${test_name}.log"
                "${SQUAD_ANDROID_TEST_RUNNER}"
                --path "${package}" --adb "${SQUAD_ADB}" --timeout 180
                --apk "${package}/${target}.apk" --show-logcat --skip-install-root
                --make "\"${CMAKE_COMMAND}\" --build \"${CMAKE_BINARY_DIR}\" --target ${target}_make_apk"
                -- ${ARGN})
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 300 RESOURCE_LOCK android_emulator
                ENVIRONMENT "QT_ANDROID_NO_EXIT_CALL=1"
                FAIL_REGULAR_EXPRESSION "FAIL!|QFATAL|FORTIFY:|Fatal signal")
        endfunction()
        squad_add_android_test(controls_tests channel_controls
            -input :/tests/tst_channel_controls.qml
            DirectChannelControls::test_platformSpecificLanguageControls
            DirectChannelControls::test_dockPreferenceIsMacOnlyAndKeepsVoiceState
            DirectChannelControls::test_chatSendsOnlyAfterHostReceiptAndKeepsVoiceAvailable:English
            DirectChannelControls::test_all64MembersAreReachableFromChannelSummary
            DirectChannelControls::test_returningToChatPaintsWithoutScrolling
            DirectChannelControls::test_settingsContainProfileAndAudioInTheChannelWindow
            DirectChannelControls::test_scrollLoadsOlderMessagesWithoutDownloadingThemAtOpen
            DirectChannelControls::test_scrollbarTracksLoadedMessagesWithoutResizing
            DirectChannelControls::test_chatKeepsReadingPositionAcrossResize
            DirectChannelControls::test_wheelScrollsInsideOversizedMessage
            DirectChannelControls::test_touchScrollsChatAndReleasesPtt)
        set_property(TEST channel_controls APPEND PROPERTY ENVIRONMENT "QT_ANDROID_NO_FULLSCREEN_KEYBOARD=1")
        squad_add_android_test(channel_tests channel_contract
            tlsWithoutDeviceCertificateCannotReachAdmission
            headlessAutomaticallyAdmitsUnapprovedClientsExceptBans
            ownerCanKickBanAndUnblockWithoutJoining
            passwordsPreserveActiveMembersAndPersistPerHostSecrets
            remotePttNeedsSeparateApprovalAndCombinesControllers
            chatSurvivesHostRestartAndReplaysToNewApprovedParticipants)
        squad_add_android_test(mixer_tests mixer_contract)
        squad_add_android_test(media_transport_tests media_transport_contract)
        squad_add_android_test(video_tests video_contract
            roundTripAndQualityChanges nativePixelFormatsKeepColorsAcrossChanges
            invalidFramesAndFormatsFailBoundedly udpScreenRoundTripDecodesRealFrames)
        return()
    endif()
    set(desktop_test_command $<TARGET_FILE:squadspeak>)
    if(WIN32)
        list(PREPEND desktop_test_command ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_with_deadline.py"
            --timeout 15 --dump "${CMAKE_BINARY_DIR}/smoke/startup.dmp")
    endif()
    add_test(NAME desktop_smoke COMMAND ${desktop_test_command} --smoke-test
        --settings-file "${CMAKE_BINARY_DIR}/smoke/audio.ini"
        --screenshot "${CMAKE_BINARY_DIR}/smoke/audio.png")
    set_tests_properties(desktop_smoke PROPERTIES
        TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
    if(WIN32)
        # The child still has 15 seconds. Allow a bounded dump and cleanup on failure.
        # Check cold QML startup before the long suite, without competing workloads.
        set_tests_properties(desktop_smoke PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE COST 2000)
        set_property(TEST desktop_smoke APPEND PROPERTY ENVIRONMENT QML_IMPORT_TRACE=1)
    endif()
    add_test(NAME process_deadline COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_deadline_tests.py"
        --controls-tests $<TARGET_FILE:controls_tests>)
    set_tests_properties(process_deadline PROPERTIES TIMEOUT 30)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_version_tests.py"
        --compile-commands "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json")
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
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
        add_test(NAME output_monitor_retry_contract COMMAND capture_tests
            outputMonitorDoesNotRetryFailedCaptureAutomatically outputMonitorDoesNotRepeatInterruptedStartup)
        set_tests_properties(output_monitor_retry_contract PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
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
    add_test(NAME audio_contract COMMAND audio_tests)
    add_test(NAME mixer_contract COMMAND mixer_tests)
    qt_add_executable(corpus_tests tests/corpus_tests.cpp)
    target_link_libraries(corpus_tests PRIVATE squad_audio Qt6::Test)
    target_compile_definitions(corpus_tests PRIVATE
        SQUAD_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/audio"
        SQUAD_CORPUS_OUTPUT="${CMAKE_BINARY_DIR}/audio-corpus")
    if(SQUADSPEAK_DENOISE_CANDIDATE)
        add_test(NAME audio_corpus COMMAND corpus_tests mixtures)
        foreach(part speechActivation quietWordOnsets repeatedSpeech automaticInput playbackEcho duplexEchoDecays)
            add_test(NAME audio_corpus_${part} COMMAND corpus_tests ${part})
            set_tests_properties(audio_corpus_${part} PROPERTIES TIMEOUT 600
                ENVIRONMENT "SQUAD_CORPUS_OUTPUT=${CMAKE_BINARY_DIR}/audio-corpus-${part}")
        endforeach()
    else()
        add_test(NAME audio_corpus COMMAND corpus_tests)
    endif()
    # Instrumented inference is much slower; this is not a performance gate.
    set_tests_properties(audio_corpus PROPERTIES TIMEOUT 600)
    qt_add_executable(radio_tests tests/radio_tests.cpp)
    target_link_libraries(radio_tests PRIVATE squad_radio Qt6::Test)
    add_test(NAME radio_contract COMMAND radio_tests)
    # The reconnect and paced playback cases take 169 s on hosted Linux;
    # allow slower runners to complete the same per-operation assertions.
    set_tests_properties(radio_contract PROPERTIES TIMEOUT 240)
    qt_add_executable(session_tests tests/session_tests.cpp)
    target_link_libraries(session_tests PRIVATE squad_session Qt6::Test OpenSSL::Crypto)
    target_compile_definitions(session_tests PRIVATE SQUAD_TRANSLATION_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ui/i18n")
    add_test(NAME session_contract COMMAND session_tests)
    qt_add_executable(teamspeak_tests tests/teamspeak_tests.cpp)
    target_link_libraries(teamspeak_tests PRIVATE squad_session squad_content squad_audio Qt6::Test)
    add_executable(teamspeak_live EXCLUDE_FROM_ALL tests/teamspeak_live.cpp)
    target_link_libraries(teamspeak_live PRIVATE squad_session squad_content squad_audio Qt6::Test)
    add_test(NAME teamspeak_contract COMMAND teamspeak_tests)
    set_tests_properties(teamspeak_contract PROPERTIES TIMEOUT 45 RESOURCE_LOCK channel_fixed_ports)
    qt_add_executable(license_tests tests/license_tests.cpp src/headless.cpp src/headless.hpp)
    target_link_libraries(license_tests PRIVATE squad_radio Qt6::Test qt6keychain)
    add_test(NAME license_contract COMMAND license_tests)
    set_tests_properties(license_contract PROPERTIES TIMEOUT 120)
    qt_add_executable(ptt_key_tests tests/ptt_key_tests.cpp src/ptt_key.cpp src/ptt_key.hpp)
    target_link_libraries(ptt_key_tests PRIVATE squad_session Qt6::Gui Qt6::Test ${squad_ptt_platform_libraries})
    add_test(NAME ptt_key_contract COMMAND ptt_key_tests)
    qt_add_executable(chat_history_tests tests/chat_history_tests.cpp)
    target_link_libraries(chat_history_tests PRIVATE squad_session squad_content Qt6::Test)
    add_test(NAME chat_history_contract COMMAND chat_history_tests)
    add_test(NAME media_transport_contract COMMAND media_transport_tests)
    # RTT and packet expiry assertions need an uncontended realtime scheduler.
    set_tests_properties(media_transport_contract PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1000)
    # QtTest exposes the complete function inventory through its public
    # -functions option. Each function invocation executes all of its data
    # rows, while CTest can schedule independent functions separately.
    set(network_test_launcher)
    if(APPLE AND "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
        # Hosted runners lack interactive LAN consent (runner-images#10924).
        # Only network contracts use the root exemption. CTest stays unprivileged.
        find_program(SQUAD_SUDO sudo REQUIRED)
        set(network_test_launcher "${SQUAD_SUDO}" -n
            --preserve-env=QT_QPA_PLATFORM,QT_QUICK_BACKEND,QT_QUICK_CONTROLS_STYLE,SQUAD_TEST_ARTIFACTS,QV4_JIT_CALL_THRESHOLD,QV4_GC_TIMELIMIT,SQUAD_SOAK_CLIENTS,SQUAD_SOAK_SPEAKERS,SQUAD_SOAK_SECONDS
            env "QT_LOGGING_RULES=")
    endif()
    set(channel_discovery_file "${CMAKE_CURRENT_BINARY_DIR}/channel_contract_discovered.cmake")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${channel_discovery_file}")
    set_property(TARGET channel_tests APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DiscoverChannelTests.cmake")
    add_custom_command(TARGET channel_tests POST_BUILD
        BYPRODUCTS "${channel_discovery_file}"
        COMMAND "${CMAKE_COMMAND}"
            -DCHANNEL_TEST_EXECUTABLE=$<TARGET_FILE:channel_tests>
            -DOUTPUT_FILE=${channel_discovery_file}
            "-DTEST_LAUNCHER=${network_test_launcher}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DiscoverChannelTests.cmake"
        VERBATIM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        qt_add_executable(discovery_network_probe tests/discovery_network_probe.cpp)
        target_link_libraries(discovery_network_probe PRIVATE squad_session squad_content)
    endif()
    qt_add_executable(headless_tests tests/headless_tests.cpp src/headless.cpp src/headless.hpp)
    target_link_libraries(headless_tests PRIVATE squad_radio squad_session Qt6::Core Qt6::Test qt6keychain)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_link_libraries(headless_tests PRIVATE Qt6::Gui)
    endif()
    target_compile_definitions(headless_tests PRIVATE SQUAD_HEADLESS_APP="$<TARGET_FILE:squadspeak>"
        SQUAD_TEST_PYTHON="${Python3_EXECUTABLE}")
    add_dependencies(headless_tests squadspeak)
    add_test(NAME headless_contract COMMAND headless_tests)
    # The suite launches dozens of real processes; per-operation deadlines stay
    # in the tests, while this outer budget also allows loaded CI runners.
    set_tests_properties(headless_contract PROPERTIES TIMEOUT 240)
    add_test(NAME audio_controls COMMAND controls_tests -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_controls.qml")
    set_tests_properties(audio_controls PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
    add_test(NAME frequency_display COMMAND controls_tests -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_spectrum.qml")
    set_tests_properties(frequency_display PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/smoke")
    set(controls_test_command $<TARGET_FILE:controls_tests>)
    if(network_test_launcher)
        list(PREPEND controls_test_command "${CMAKE_COMMAND}" -E env ${network_test_launcher})
    endif()
    add_test(NAME channel_controls COMMAND ${controls_test_command} -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_channel_controls.qml"
        -o "${CMAKE_BINARY_DIR}/smoke/channel-controls.xml,junitxml"
        -o "${CMAKE_BINARY_DIR}/smoke/channel-controls-timing.xml,xml" -o -,txt)
    set_tests_properties(channel_controls PROPERTIES
        TIMEOUT 600 RUN_SERIAL TRUE
        ENVIRONMENT "QT_QPA_PLATFORM=${ui_test_platform};QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;SQUAD_TEST_ARTIFACTS=${CMAKE_BINARY_DIR}/smoke;QV4_JIT_CALL_THRESHOLD=1;QV4_GC_TIMELIMIT=1")
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
        set_tests_properties(desktop_smoke desktop_rendering_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Could not find permission plugin for QMicrophonePermission")
    endif()
    # Keep timing/load measurements and native loopback capture undisturbed.
    set_tests_properties(video_contract PROPERTIES RUN_SERIAL TRUE)
    if(TEST capture_contract)
        set_tests_properties(capture_contract PROPERTIES RUN_SERIAL TRUE)
    endif()
    set_tests_properties(channel_controls radio_contract audio_corpus PROPERTIES COST 500)
    # GUI, clipboard and audio-device fixtures share the runner's desktop.
    set_tests_properties(headless_contract desktop_smoke process_deadline
        audio_controls frequency_display channel_controls avatar_scale_1.5 avatar_scale_2
        screen_rendering desktop_rendering_smoke PROPERTIES RESOURCE_LOCK desktop)
    if(TEST linux_global_ptt)
        set_tests_properties(linux_global_ptt PROPERTIES RESOURCE_LOCK desktop)
    endif()
endif()
