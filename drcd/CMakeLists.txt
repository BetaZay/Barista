add_library(drcd_core STATIC
    src/interrupt.cpp
    src/media_streamer.cpp
    src/server_core.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(drcd_core PRIVATE src/backend_linux.cpp)
    if(TARGET drc_hostapd)
        add_dependencies(drcd_core drc_hostapd)
        target_compile_definitions(drcd_core PRIVATE
            DRCD_DEFAULT_HOSTAPD_BIN="${DRC_HOSTAPD_BINARY}")
    endif()
else()
    target_sources(drcd_core PRIVATE src/backend_noop.cpp)
endif()

target_include_directories(drcd_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(drcd_core
    PUBLIC
        drc_host
        drc_ipc
    PRIVATE
        drc_x264
        m
        pthread
        dl
)

target_compile_features(drcd_core PUBLIC cxx_std_20)

add_executable(drcd
    src/main.cpp
)

target_link_libraries(drcd
    PRIVATE
        drcd_core
        drc_host
)

target_compile_features(drcd PRIVATE cxx_std_20)

option(DRCD_BUILD_TESTS "Build drcd unit tests" ON)
if(DRCD_BUILD_TESTS)
    add_executable(drcd_serial_sender_tests tests/serial_video_sender_tests.cpp)
    target_compile_features(drcd_serial_sender_tests PRIVATE cxx_std_20)
    target_link_libraries(drcd_serial_sender_tests PRIVATE pthread)
    add_test(NAME drcd_serial_sender_tests COMMAND drcd_serial_sender_tests)
    set_tests_properties(drcd_serial_sender_tests PROPERTIES TIMEOUT 10)
    add_executable(drcd_core_tests
        tests/server_core_tests.cpp
    )
    target_link_libraries(drcd_core_tests PRIVATE drcd_core)
    target_compile_features(drcd_core_tests PRIVATE cxx_std_20)
    add_test(NAME drcd_core_tests COMMAND drcd_core_tests)
    add_test(NAME drcd_fast_encode_tests COMMAND drcd_core_tests)
    set_tests_properties(drcd_fast_encode_tests PROPERTIES ENVIRONMENT "DRCD_FAST_ENCODE=1")
    add_test(NAME drcd_no_intra_refresh_tests COMMAND drcd_core_tests)
    set_tests_properties(drcd_no_intra_refresh_tests PROPERTIES ENVIRONMENT "DRCD_INTRA_REFRESH=0")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(drcd_media_bridge_tests tests/media_bridge_stream_tests.cpp)
        target_link_libraries(drcd_media_bridge_tests PRIVATE drcd_core)
        add_test(NAME drcd_media_bridge_tests COMMAND drcd_media_bridge_tests)
        set_tests_properties(drcd_media_bridge_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
        add_test(NAME drcd_no_intra_refresh_bridge_tests COMMAND drcd_media_bridge_tests)
        set_tests_properties(drcd_no_intra_refresh_bridge_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
            ENVIRONMENT "DRCD_INTRA_REFRESH=0")
        add_test(NAME drcd_recovery_spacing_tests COMMAND drcd_media_bridge_tests)
        set_tests_properties(drcd_recovery_spacing_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
            ENVIRONMENT "DRCD_IDR_PAUSE=1;DRCD_IDR_INIT=1;DRCD_SEND_TIME_VIDEO=1;DRCD_VIDEO_QP=32")
        add_test(NAME drcd_legacy_pacing_tests COMMAND drcd_media_bridge_tests)
        set_tests_properties(drcd_legacy_pacing_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
            ENVIRONMENT "DRCD_CHUNK_PACING=0;DRCD_IDR_INIT=0;DRCD_SEND_TIME_VIDEO=0;DRCD_IDR_PAUSE=0;DRCD_VIDEO_QP=32")
    endif()
endif()
