cmake_minimum_required(VERSION 3.20)
project(drc_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(third_party/hostapd-drc)
endif()

add_subdirectory(third_party/drc-x264)

add_subdirectory(libdrc-host)
add_subdirectory(libdrc-ipc)
add_subdirectory(drcd)
add_subdirectory(drcctl)
