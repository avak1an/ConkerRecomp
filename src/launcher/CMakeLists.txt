cmake_minimum_required(VERSION 3.20)
project(conker_launcher LANGUAGES C)
if(NOT WIN32)
    message(FATAL_ERROR "The launcher currently requires Windows.")
endif()
add_executable(conker-launcher WIN32 launcher.c title_art.c)
set_target_properties(conker-launcher PROPERTIES C_STANDARD 11
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_definitions(conker-launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(conker-launcher PRIVATE /W3 /O2 /utf-8)
    target_link_options(conker-launcher PRIVATE
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/launcher.manifest"
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()
target_link_libraries(conker-launcher PRIVATE comctl32 comdlg32 shell32 ole32
    user32 gdi32 xinput9_1_0)
option(CONKER_LAUNCHER_TEST "Build the launcher UI smoke-test driver" OFF)
if(CONKER_LAUNCHER_TEST)
    target_compile_definitions(conker-launcher PRIVATE CONKER_LAUNCHER_TEST)
    add_executable(conker-title-art-test title_art_test.c title_art.c)
    set_target_properties(conker-title-art-test PROPERTIES C_STANDARD 11)
    target_link_libraries(conker-title-art-test PRIVATE user32 gdi32)
endif()
