# Store builds use their store updater. Direct macOS builds use Sparkle with
# a pinned public key; an unconfigured developer build never contacts a feed.
set(SQUADSPEAK_UPDATE_PUBLIC_KEY "" CACHE STRING "Base64 Ed25519 public update key")
set(squad_update_plist "")
if(SQUADSPEAK_UPDATE_PUBLIC_KEY)
    string(LENGTH "${SQUADSPEAK_UPDATE_PUBLIC_KEY}" update_key_length)
    if(NOT update_key_length EQUAL 44 OR NOT SQUADSPEAK_UPDATE_PUBLIC_KEY MATCHES "^[A-Za-z0-9+/]+=$")
        message(FATAL_ERROR "The update public key must be a base64-encoded 32-byte Ed25519 key")
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT SQUADSPEAK_STORE_BUILD)
    FetchContent_Declare(sparkle
        URL https://github.com/sparkle-project/Sparkle/releases/download/2.10.0/Sparkle-2.10.0.tar.xz
        URL_HASH SHA256=c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sparkle)
    add_library(squad_updater INTERFACE)
    target_link_libraries(squad_updater INTERFACE "${sparkle_SOURCE_DIR}/Sparkle.framework")
    target_compile_options(squad_updater INTERFACE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_compile_definitions(squad_updater INTERFACE SQUADSPEAK_UPDATES=1)
    if(NOT SQUADSPEAK_UPDATE_PUBLIC_KEY)
        return()
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
        set(update_architecture "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(update_architecture "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT update_architecture MATCHES "^(arm64|x86_64)$")
        message(FATAL_ERROR "The update feed requires a single supported macOS architecture")
    endif()
    set(squad_update_plist "
    <key>SUFeedURL</key><string>https://github.com/YunaBraska/SquadSpeak/releases/latest/download/appcast-macos-${update_architecture}.xml</string>
    <key>SUPublicEDKey</key><string>${SQUADSPEAK_UPDATE_PUBLIC_KEY}</string>
    <key>SUEnableAutomaticChecks</key><true/>
    <key>SUAutomaticallyUpdate</key><false/>
    <key>SUAllowsAutomaticUpdates</key><false/>
    <key>SUVerifyUpdateBeforeExtraction</key><true/>
    <key>SURequireSignedFeed</key><true/>")
endif()
