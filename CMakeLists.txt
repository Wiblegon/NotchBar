cmake_minimum_required(VERSION 3.24)
project(NotchBar LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the release exe has no Visual C++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /W4 /wd4100 /wd4505 /EHsc /bigobj)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

# ---- Model + core: no window/GPU code, shared by the app and the tests ----
add_library(nb_model STATIC
    src/core/Json.cpp
    src/core/Strings.cpp
    src/core/AtomicFile.cpp
    src/core/Paths.cpp
    src/model/Calc.cpp
    src/model/VisibilityModel.cpp
    src/model/PanelDirection.cpp
    src/model/Favorites.cpp
    src/model/SettingsModel.cpp
    src/model/Battery.cpp
    src/model/WebSearch.cpp
    src/model/ScreenClassifier.cpp
    src/model/Notifications.cpp
    src/model/Search.cpp
)
target_include_directories(nb_model PUBLIC src)
target_link_libraries(nb_model PUBLIC shell32 ole32)

option(NB_APP "Build the application" ON)
if(NB_APP)
file(GLOB_RECURSE NB_APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/app/*.cpp
    ${CMAKE_SOURCE_DIR}/src/gfx/*.cpp
    ${CMAKE_SOURCE_DIR}/src/sys/*.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/*.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Log.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Dispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Worker.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Store.cpp)
add_executable(NotchBar WIN32 ${NB_APP_SOURCES} src/app/resources.rc)
target_link_libraries(NotchBar PRIVATE nb_model
    d3d11 dxgi dxguid d2d1 dwrite dcomp windowscodecs dwmapi shcore shlwapi comctl32 uxtheme
    oleaut32 propsys windowsapp taskschd hid setupapi cfgmgr32 iphlpapi wlanapi
    wtsapi32 version advapi32 user32 gdi32 powrprof)
target_precompile_headers(NotchBar PRIVATE src/pch.h)
target_link_options(NotchBar PRIVATE /MANIFEST:NO)
endif()

# ---- Tests ----
enable_testing()
add_executable(nb_tests tests/test_main.cpp tests/test_model.cpp)
target_link_libraries(nb_tests PRIVATE nb_model)
add_test(NAME model_tests COMMAND nb_tests)

# ---- Borderless fullscreen frame-time probe (stand-in "game" for measurements) ----
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/fsprobe/fsprobe.cpp)
add_executable(fsprobe WIN32 tools/fsprobe/fsprobe.cpp)
target_link_libraries(fsprobe PRIVATE d3d11 dxgi user32 shell32)
endif()
