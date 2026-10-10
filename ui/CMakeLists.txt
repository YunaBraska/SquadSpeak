qt_add_executable(squadspeak MACOSX_BUNDLE ../src/main.cpp ../src/headless.cpp ../src/headless.hpp ../src/audio_model.cpp ../src/audio_model.hpp ../src/audio_recording.cpp ../src/audio_recording.hpp ../src/ptt_key.cpp ../src/ptt_key.hpp)
set(squad_qml_files Theme.qml PanelWindow.qml PanelDialog.qml TextToolTip.qml ActionButton.qml EntryField.qml ChoiceBox.qml OptionCheck.qml Glyph.qml AudioSettings.qml RecordingPanel.qml Channels.qml ChatPanel.qml VoiceAvatar.qml AvatarAtlas.js SettingSlider.qml SpectrumView.qml ScreenPanel.qml)
set(squad_resource_files icons/app.png avatars/mossling.png avatars/courier.png avatars/mechanic.png avatars/system.png avatars/system-animated.png
    avatars/ember-dragon.png
    avatars/moon-owl.png
    avatars/mushroom-sage.png
    avatars/coral-diver.png
    avatars/clockwork-beetle.png
    avatars/crystal-golem.png
    avatars/cloud-shepherd.png
    avatars/star-moth.png
    avatars/lantern-ghost.png
    avatars/desert-fennec.png
    avatars/octopus-pilot.png
    avatars/snail-captain.png
    avatars/orchid-mantis.png
    avatars/comet-axolotl.png
    avatars/frost-yeti.png
    avatars/raccoon-alchemist.png
    avatars/sunflower-knight.png)
foreach(squad_file IN LISTS squad_qml_files squad_resource_files)
    set_source_files_properties("${squad_file}" PROPERTIES QT_RESOURCE_ALIAS "ui/${squad_file}")
endforeach()
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(squadspeak URI SquadSpeak VERSION 1.0
    QML_FILES ${squad_qml_files} RESOURCES ${squad_resource_files})
target_link_libraries(squadspeak PRIVATE squad_video squad_radio squad_audio squad_session squad_content Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia ${squad_ptt_platform_libraries})
if(NOT IOS)
    add_dependencies(squadspeak squad_image_worker)
endif()
if(TARGET squad_updater)
    target_sources(squadspeak PRIVATE ../src/app_updates.mm ../src/app_updates.hpp)
    target_link_libraries(squadspeak PRIVATE squad_updater)
endif()
if(APPLE)
    set(squad_bundle_localizations "")
    foreach(language en ${squad_languages})
        string(APPEND squad_bundle_localizations "<string>${language}</string>")
        set(permission_text "i18n/${language}.lproj/InfoPlist.strings")
        set_source_files_properties("${permission_text}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/${language}.lproj")
        target_sources(squadspeak PRIVATE "${permission_text}")
    endforeach()
    target_sources(squadspeak PRIVATE ../src/apple_notifications.mm ../src/apple_notifications.hpp)
    set_source_files_properties(../src/apple_notifications.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(squadspeak PRIVATE "-framework UserNotifications")
    if(IOS)
        set(squad_bundle_icon icons/app.png)
    else()
        set(squad_bundle_icon icons/app.icns)
    endif()
    set_source_files_properties(${squad_bundle_icon} sounds/announcement.wav PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(squadspeak PRIVATE ${squad_bundle_icon} sounds/announcement.wav)
    if(NOT IOS)
        # macdeployqt rewrites codec paths when creating the desktop package.
        set_property(TARGET squadspeak PROPERTY INSTALL_RPATH
            "@executable_path/../Frameworks;${OPUS_LIBRARY_DIRS};${SAMPLERATE_LIBRARY_DIRS}")
        add_custom_command(TARGET squadspeak POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:squad_image_worker>"
                "$<TARGET_BUNDLE_DIR:squadspeak>/Contents/MacOS/squad_image_worker")
    endif()
endif()
if(WIN32)
    target_sources(squadspeak PRIVATE icons/app.rc ../src/windows.manifest)
    target_include_directories(squadspeak PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/icons")
endif()
if(ANDROID)
    set_target_properties(squadspeak PROPERTIES
        QT_ANDROID_PACKAGE_NAME "app.squadspeak.mobile"
        QT_ANDROID_APP_NAME "SquadSpeak"
        QT_ANDROID_TARGET_SDK_VERSION 36
        QT_ANDROID_APP_ICON "@drawable/icon"
        QT_ANDROID_PACKAGE_SOURCE_DIR "${squad_android_package}"
        QT_ANDROID_EXTRA_LIBS "${squad_android_libraries}")
endif()
# Qt reads this template before CMake generates the bundle to link both the
# permission plugin and its request entrypoint. Expand XML fragments now while
# leaving CMake's ${MACOSX_BUNDLE_*} placeholders for bundle generation.
if(IOS)
    set(squad_platform_plist "<key>LSRequiresIPhoneOS</key><true/>
        <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
        <key>UILaunchScreen</key><dict/>
        <key>UISupportedInterfaceOrientations</key><array>
            <string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationPortraitUpsideDown</string>
            <string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
        <key>UIBackgroundModes</key><array><string>audio</string></array>
        <key>CFBundleIconFiles</key><array><string>app.png</string></array>")
    set_target_properties(squadspeak PROPERTIES XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
else()
    set(squad_platform_plist "<key>CFBundleIconFile</key><string>app.icns</string>
        <key>LSUIElement</key><true/>
        <key>LSMinimumSystemVersion</key><string>\${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
endif()
configure_file(../src/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist.in" @ONLY)
set_target_properties(squadspeak PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.squadspeak.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "SquadSpeak"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist.in")
