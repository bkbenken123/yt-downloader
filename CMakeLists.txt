cmake_minimum_required(VERSION 3.24)
project(Downloader VERSION 2026.11.17 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Network)

add_executable(downloader WIN32
    src/main.cpp
    src/appconfig.h
    src/process.h src/process.cpp
    src/toolmanager.h src/toolmanager.cpp src/toolmanager_updates.cpp src/toolmanager_windows.cpp src/toolmanager_windows_msvc.cpp
    src/mediaengine.h src/mediaengine.cpp src/mediaengine_convert.cpp
    src/downloadworker.h src/downloadworker.cpp
    src/mainwindow.h src/mainwindow.cpp
)
target_link_libraries(downloader PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
target_include_directories(downloader PRIVATE src)

if(MSVC)
    target_compile_options(downloader PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(downloader PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_compile_options(downloader PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS downloader RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
add_test(NAME downloader_help COMMAND downloader --help)
