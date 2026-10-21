set(DRC_HOSTAPD_REPOSITORY "https://github.com/vanilla-wiiu/drc-hostap.git" CACHE STRING
    "Git repository used for the standalone GamePad hostapd build")
# Pin the Vanilla fork rather than tracking its moving master branch.
set(DRC_HOSTAPD_TAG "a2f705daa143791180754100194c91714f7e8801" CACHE STRING
    "Pinned GamePad hostapd commit used by Barista")

set(drc_hostapd_root "${CMAKE_CURRENT_BINARY_DIR}")
set(drc_hostapd_source "${drc_hostapd_root}/src")
set(drc_hostapd_binary "${drc_hostapd_root}/bin/drc-hostapd")
set(drc_hostapd_cli_binary "${drc_hostapd_root}/bin/drc-hostapd_cli")
# Apply only the Wii U HT advertisement patch. The other legacy patches remain
# inactive; the fork provides its own Nintendo implementation.
set(drc_hostapd_ht_patch "${CMAKE_CURRENT_SOURCE_DIR}/patches/0007-hostapd-match-Wii-U-HT-advertisement.patch")
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
            "${drc_hostapd_ht_patch}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/build_hostapd_drc.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/hostapd.config"
        "${drc_hostapd_root}/build-inputs.txt"
        "${drc_hostapd_ht_patch}"
    COMMENT "Building Vanilla GamePad hostapd"
    VERBATIM
    USES_TERMINAL
)

add_custom_target(drc_hostapd ALL
    DEPENDS "${drc_hostapd_binary}" "${drc_hostapd_cli_binary}")

set(DRC_HOSTAPD_BINARY "${drc_hostapd_binary}" PARENT_SCOPE)
set(DRC_HOSTAPD_CLI_BINARY "${drc_hostapd_cli_binary}" PARENT_SCOPE)
