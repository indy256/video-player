cmake_minimum_required(VERSION 3.21)
project(VideoPlayerPortable LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(../../cmake/ReleaseOptimizations.cmake)
file(TO_CMAKE_PATH "${RUNTIME_DIR}" RUNTIME_DIR)
if(NOT WIN32 OR NOT IS_DIRECTORY "${RUNTIME_DIR}/bin")
    message(FATAL_ERROR "Windows and RUNTIME_DIR containing bin/VideoPlayer.exe are required")
endif()
add_executable(pack-runtime pack.cpp)
target_link_libraries(pack-runtime PRIVATE cabinet)
file(GLOB_RECURSE runtime_files CONFIGURE_DEPENDS "${RUNTIME_DIR}/*")
add_custom_command(OUTPUT payload.rc payload.h payload.bin
    COMMAND pack-runtime "${RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/payload.bin"
    COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake"
    DEPENDS pack-runtime ${runtime_files} embed.cmake ../../assets/app-icon.ico
    VERBATIM)
add_executable(VideoPlayer WIN32 launcher.cpp payload.rc payload.h)
target_include_directories(VideoPlayer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(VideoPlayer PRIVATE cabinet shell32 ole32 uuid)
foreach(target pack-runtime VideoPlayer)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MINGW)
        target_link_options(${target} PRIVATE -static -municode)
    endif()
endforeach()
