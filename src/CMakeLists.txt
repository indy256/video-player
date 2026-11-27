qt_add_executable(VideoPlayer WIN32 MACOSX_BUNDLE main.cpp)
set_target_properties(VideoPlayer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set_source_files_properties(video/Video.qml PROPERTIES QT_RESOURCE_ALIAS Video.qml)
qt_add_qml_module(VideoPlayer URI PlayerVideo RESOURCE_PREFIX /qt/qml NO_CACHEGEN NO_LINT NO_PLUGIN
    QML_FILES video/Video.qml)
qt_add_resources(VideoPlayer app_icons PREFIX "/" BASE "${PROJECT_SOURCE_DIR}"
    FILES "${PROJECT_SOURCE_DIR}/assets/app-icon.png")
target_link_libraries(VideoPlayer PRIVATE player_ui)

if(WIN32)
    target_sources(VideoPlayer PRIVATE "${PROJECT_SOURCE_DIR}/assets/app-icon.rc")
    target_include_directories(VideoPlayer PRIVATE "${PROJECT_SOURCE_DIR}/assets")
endif()
if(VIDEO_PLAYER_STATIC_WINDOWS)
    # Keep Qt from searching the build kit's absolute install paths at runtime.
    qt_add_resources(VideoPlayer qt_paths PREFIX "/qt/etc" BASE "${PROJECT_SOURCE_DIR}/packaging"
        FILES "${PROJECT_SOURCE_DIR}/packaging/qt.conf")
    qt_import_plugins(VideoPlayer INCLUDE Qt6::QWindowsIntegrationPlugin
        INCLUDE_BY_TYPE multimedia Qt6::QFFmpegMediaPlugin
        INCLUDE_BY_TYPE tls Qt6::QSchannelBackendPlugin
        EXCLUDE_BY_TYPE qmltooling)
endif()
if(APPLE)
    set_target_properties(VideoPlayer PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/Info.plist.in")
    set(app_icon "${PROJECT_SOURCE_DIR}/assets/app-icon.icns")
    if(EXISTS "${app_icon}")
        target_sources(VideoPlayer PRIVATE "${app_icon}")
        set_source_files_properties("${app_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(VideoPlayer PROPERTIES MACOSX_BUNDLE_ICON_FILE app-icon.icns)
    endif()
endif()

if(BUILD_TESTING)
    qt_add_executable(player_tests "${PROJECT_SOURCE_DIR}/tests/player_tests.cpp")
    target_link_libraries(player_tests PRIVATE player_ui Qt6::Test)
    set_target_properties(player_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    if(VIDEO_PLAYER_STATIC_WINDOWS)
        qt_import_plugins(player_tests INCLUDE Qt6::QOffscreenIntegrationPlugin
            INCLUDE_BY_TYPE multimedia Qt6::QFFmpegMediaPlugin
            EXCLUDE_BY_TYPE qmltooling)
    endif()
endif()
