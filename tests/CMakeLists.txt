# Keep static QML plugin scanning within the test sources. Scanning the
# repository root also reads unrelated QML from local dependency builds.
qt_add_executable(controls_tests controls_tests.cpp
    ../src/ptt_key.cpp ../src/ptt_key.hpp
    ../src/audio_model.cpp ../src/audio_model.hpp
    ../src/audio_recording.cpp ../src/audio_recording.hpp)
set_target_properties(controls_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
if(QT6_IS_SHARED_LIBS_BUILD)
    set_property(TARGET controls_tests PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
else()
    set_property(TARGET controls_tests PROPERTY QT_QML_IMPORT_SCANNER_EXTRA_ROOT_PATHS "${PROJECT_SOURCE_DIR}/ui")
endif()
target_link_libraries(controls_tests PRIVATE squad_video squad_radio squad_audio
    squad_session squad_content Qt6::QuickTest Qt6::QuickControls2
    Qt6::Multimedia ${squad_ptt_platform_libraries})
