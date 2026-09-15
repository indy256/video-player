# MinGW GCC misidentifies COMDAT thunks in Qt's multiple-inheritance
# accessibility and window classes during LTO. Keep these translation units native.
function(qt_mingw_lto_exceptions)
    foreach(plugin IN ITEMS QWindowsIntegrationPlugin QWindowsDirect2DIntegrationPlugin)
        if(TARGET ${plugin})
            set_target_properties(${plugin} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
        endif()
    endforeach()
    file(GLOB accessibility_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/widgets/accessible/*.cpp")
    set_source_files_properties(${accessibility_sources}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/widgets/kernel/qwidgetwindow.cpp"
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/widgets"
        PROPERTIES COMPILE_OPTIONS -fno-lto)
endfunction()

cmake_language(DEFER CALL qt_mingw_lto_exceptions)
