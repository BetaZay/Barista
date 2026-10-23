set(DRC_HOSTAPD_REPOSITORY "https://github.com/vanilla-wiiu/drc-hostap.git" CACHE STRING
    "Git repository used for the standalone GamePad hostapd build")
# Pin the Vanilla fork rather than tracking its moving master branch.
set(DRC_HOSTAPD_TAG "a2f705daa143791180754100194c91714f7e8801" CACHE STRING
    "Pinned GamePad hostapd commit used by Barista")

set(drc_hostapd_root "${CMAKE_CURRENT_BINARY_DIR}")
set(drc_hostapd_source "${drc_hostapd_root}/src")
set(drc_hostapd_binary "${drc_hostapd_root}/bin/drc-hostapd")
set(drc_hostapd_cli_binary "${drc_hostapd_root}/bin/drc-hostapd_cli")
# The fork supplies Nintendo crypto, but still needs the Wii U HT and WPS
# advertisement/receive compatibility patches.
set(drc_hostapd_patches
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/0007-hostapd-match-Wii-U-HT-advertisement.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/0015-restore-Nintendo-WPS-discovery.patch")
file(GENERATE OUTPUT "${drc_hostapd_root}/build-inputs.txt"
    CONTENT "${DRC_HOSTAPD_REPOSITORY}\n${DRC_HOSTAPD_TAG}\n")

add_custom_command(
    OUTPUT "${drc_hostapd_binary}" "${drc_hostapd_cli_binary}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/build_hostapd_drc.sh"
            "${drc_hostapd_source}"
            "${DRC_HOSTAPD_REPOSITORY}"
            "${DRC_HOSTAPD_TAG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/hostapd.config"
            "${drc_hostapd_binary}"
            "${drc_hostapd_cli_binary}"
            ${drc_hostapd_patches}
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/build_hostapd_drc.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/hostapd.config"
        "${drc_hostapd_root}/build-inputs.txt"
        ${drc_hostapd_patches}
    COMMENT "Building Vanilla GamePad hostapd"
    VERBATIM
    USES_TERMINAL
)

add_custom_target(drc_hostapd ALL
    DEPENDS "${drc_hostapd_binary}" "${drc_hostapd_cli_binary}")

if(BUILD_TESTING)
    # Exercise the actual dependency's WPS builders/parser without a radio.
    # Section GC omits unrelated protocol entry points and their dependencies.
    set(wps_test_sources
        "${drc_hostapd_source}/src/wps/wps.c"
        "${drc_hostapd_source}/src/wps/wps_attr_build.c"
        "${drc_hostapd_source}/src/common/ieee802_11_common.c"
        "${drc_hostapd_source}/src/utils/common.c"
        "${drc_hostapd_source}/src/utils/wpabuf.c"
        "${drc_hostapd_source}/src/utils/os_unix.c"
        "${drc_hostapd_source}/src/utils/wpa_debug.c")
    set_source_files_properties(${wps_test_sources} PROPERTIES GENERATED TRUE)
    foreach(profile tendonin standard)
        set(test_target "barista_hostapd_wps_${profile}_tests")
        add_executable(${test_target} "${PROJECT_SOURCE_DIR}/core/tests/hostapd_wps_tests.c" ${wps_test_sources})
        add_dependencies(${test_target} drc_hostapd)
        target_include_directories(${test_target} PRIVATE
            "${drc_hostapd_source}/src" "${drc_hostapd_source}/src/utils")
        target_compile_options(${test_target} PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(${test_target} PRIVATE -Wl,--gc-sections)
        if(profile STREQUAL "tendonin")
            target_compile_definitions(${test_target} PRIVATE CONFIG_TENDONIN)
        endif()
        add_test(NAME ${test_target} COMMAND ${test_target})
    endforeach()
endif()

set(DRC_HOSTAPD_BINARY "${drc_hostapd_binary}" PARENT_SCOPE)
set(DRC_HOSTAPD_CLI_BINARY "${drc_hostapd_cli_binary}" PARENT_SCOPE)
