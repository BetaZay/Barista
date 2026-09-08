cmake_minimum_required(VERSION 3.20)
project(Barista VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-mprefer-vector-width=128)
    add_link_options(-mprefer-vector-width=128)
endif()

# Determine Barista version from git metadata when available
if(NOT DEFINED BARISTA_VERSION OR BARISTA_VERSION STREQUAL "")
    set(BARISTA_VERSION "0.1.0")
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --match=[0-9]* --match=v[0-9]* --long
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE BARISTA_GIT_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(BARISTA_GIT_DESCRIBE)
            string(REGEX REPLACE "^v" "" BARISTA_GIT_DESCRIBE "${BARISTA_GIT_DESCRIBE}")
            string(REGEX REPLACE "-([0-9]+)-g([0-9a-f]+)$" ".r\\1.g\\2" BARISTA_GIT_DESCRIBE "${BARISTA_GIT_DESCRIBE}")
            set(BARISTA_VERSION "${BARISTA_GIT_DESCRIBE}")
        else()
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE BARISTA_GIT_COUNT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE BARISTA_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(BARISTA_GIT_COUNT AND BARISTA_GIT_HASH)
                set(BARISTA_VERSION "0.1.0.r${BARISTA_GIT_COUNT}.g${BARISTA_GIT_HASH}")
            endif()
        endif()
    endif()
endif()

message(STATUS "Barista version: ${BARISTA_VERSION}")
add_compile_definitions(BARISTA_VERSION_STRING="${BARISTA_VERSION}")

include(CTest)
include(GNUInstallDirs)
set(BARISTA_ENGINE_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BARISTA_ENGINE_DEFAULT ON)
endif()
option(BARISTA_BUILD_ENGINE "Build the Linux GamePad radio/media engine" ${BARISTA_ENGINE_DEFAULT})
option(BARISTA_BUILD_DESKTOP "Build the Qt6 Barista desktop application" ON)

if(BARISTA_BUILD_ENGINE)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The current radio engine is Linux-only. Use -DBARISTA_BUILD_ENGINE=OFF for the portable UI/core.")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(third_party/hostapd-drc)
endif()

add_subdirectory(third_party/drc-x264)
endif()
add_subdirectory(core)
if(BARISTA_BUILD_DESKTOP OR BARISTA_BUILD_ENGINE)
    add_subdirectory(app)
endif()

# Native Linux packages.  The installed service owns the privileged engine;
# the desktop binary remains a normal-user application.
set(CPACK_PACKAGE_NAME "barista")
set(CPACK_PACKAGE_VENDOR "Barista")
set(CPACK_PACKAGE_CONTACT "Barista maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Wii U GamePad desktop bridge")
set(CPACK_PACKAGE_VERSION "${BARISTA_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "barista-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "DEB;RPM")
set(CPACK_DEBIAN_FILE_NAME "barista-${CMAKE_SYSTEM_PROCESSOR}.deb")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "network-manager, policykit-1, kmod, iw, iproute2")
find_program(BARISTA_DPKG_SHLIBDEPS dpkg-shlibdeps)
if(BARISTA_DPKG_SHLIBDEPS)
    # On Debian-family builders this adds the exact Qt/ABI runtime packages.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
set(CPACK_RPM_FILE_NAME "barista-${CMAKE_SYSTEM_PROCESSOR}.rpm")
set(CPACK_RPM_PACKAGE_LICENSE "License not yet specified")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase, NetworkManager, polkit, kmod, iw, iproute")
include(CPack)
