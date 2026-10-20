# The backend's private ABI and replaced implementation must match exactly.
if(NOT WIN32 OR NOT MSVC OR NOT Qt6_VERSION VERSION_EQUAL "6.11.2")
    message(FATAL_ERROR "Early GPU scaling requires Windows, MSVC, and Qt 6.11.2")
endif()
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
get_filename_component(qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" scale_arch)
if(NOT scale_arch STREQUAL "arm64" AND NOT scale_arch STREQUAL "x64")
    message(FATAL_ERROR "Early GPU scaling requires an ARM64 or x64 Qt kit")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare.py"
    --qt-root "${qt_root}" --work "${CMAKE_CURRENT_BINARY_DIR}" --arch "${scale_arch}"
    COMMAND_ERROR_IS_FATAL ANY)
set(FFMPEG_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-sdk")
find_package(Qt6 REQUIRED COMPONENTS FFmpegMediaPluginImplPrivate)
qt_add_plugin(early_scale_ffmpeg SHARED CLASS_NAME QFFmpegMediaPlugin
    PLUGIN_TYPE multimedia OUTPUT_NAME ffmpegmediaplugin
    "${CMAKE_CURRENT_BINARY_DIR}/qffmpegplugin.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/qffmpeghwaccel_d3d11.cpp")
target_include_directories(early_scale_ffmpeg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${qt_root}/include/QtFFmpegMediaPluginImpl/${Qt6_VERSION}/QtFFmpegMediaPluginImpl/private")
target_link_libraries(early_scale_ffmpeg PRIVATE Qt6::FFmpegMediaPluginImplPrivate d3d11 dxgi dxguid)
target_compile_options(early_scale_ffmpeg PRIVATE /utf-8)
set_target_properties(early_scale_ffmpeg PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/multimedia"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/multimedia")
target_compile_definitions(VideoPlayer PRIVATE VIDEO_PLAYER_EARLY_GPU_SCALE)
add_dependencies(VideoPlayer early_scale_ffmpeg)
