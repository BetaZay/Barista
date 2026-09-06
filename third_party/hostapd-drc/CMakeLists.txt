set(DRC_HOSTAPD_REPOSITORY "https://git.w1.fi/hostap.git" CACHE STRING
    "Git repository used for the standalone patched hostapd build")
set(DRC_HOSTAPD_TAG "hostap_2_11" CACHE STRING
    "Upstream hostapd tag used by Barista")

set(drc_hostapd_root "${CMAKE_CURRENT_BINARY_DIR}")
set(drc_hostapd_source "${drc_hostapd_root}/src")
set(drc_hostapd_binary "${drc_hostapd_root}/bin/drc-hostapd")
set(drc_hostapd_cli_binary "${drc_hostapd_root}/bin/drc-hostapd_cli")
file(GLOB drc_hostapd_patches CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")

add_custom_command(
    OUTPUT "${drc_hostapd_binary}" "${drc_hostapd_cli_binary}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/build_hostapd_drc.sh"
            "${drc_hostapd_source}"
            "${DRC_HOSTAPD_REPOSITORY}"
            "${DRC_HOSTAPD_TAG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/patches"
            "${CMAKE_CURRENT_SOURCE_DIR}/hostapd.config"
            "${drc_hostapd_binary}"
            "${drc_hostapd_cli_binary}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/build_hostapd_drc.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/hostapd.config"
        ${drc_hostapd_patches}
    COMMENT "Building Barista's patched hostapd"
    VERBATIM
    USES_TERMINAL
)

add_custom_target(drc_hostapd ALL
    DEPENDS "${drc_hostapd_binary}" "${drc_hostapd_cli_binary}")

set(DRC_HOSTAPD_BINARY "${drc_hostapd_binary}" PARENT_SCOPE)
set(DRC_HOSTAPD_CLI_BINARY "${drc_hostapd_cli_binary}" PARENT_SCOPE)
