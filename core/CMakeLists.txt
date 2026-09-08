add_library(barista_api INTERFACE)
add_library(barista::api ALIAS barista_api)
target_include_directories(barista_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(barista_api INTERFACE cxx_std_20)

add_library(barista_core INTERFACE)
target_link_libraries(barista_core INTERFACE barista_api)
target_compile_features(barista_core INTERFACE cxx_std_20)

if(BARISTA_BUILD_ENGINE)
    add_library(drc_host STATIC
        drh/mac_address.cpp
        drh/pairing.cpp
        drh/runtime_transport.cpp
        drh/session.cpp
        drh/version.cpp
    )
    target_include_directories(drc_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(drc_host PUBLIC cxx_std_20)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_library(drc_ipc STATIC api/app_hook.cpp)
    else()
        add_library(drc_ipc STATIC api/app_hook_stub.cpp)
    endif()
    target_include_directories(drc_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(drc_ipc PUBLIC cxx_std_20)
    target_link_libraries(drc_ipc PUBLIC pthread)

    add_library(barista_drh_encoder STATIC
        drh/encoder/x264/bit_writer.cpp
        drh/encoder/x264/cabac.cpp
        drh/encoder/x264/contexts.cpp
        drh/encoder/x264/minih264e.cpp
        drh/encoder/media_streamer.cpp
        drh/encoder/x264/encoder.cpp
    )
    add_library(barista::drh_encoder ALIAS barista_drh_encoder)
    target_include_directories(barista_drh_encoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(barista_drh_encoder
        PUBLIC barista_api
        PRIVATE drc_host drc_ipc drc_x264 m pthread dl
    )
    target_compile_features(barista_drh_encoder PUBLIC cxx_std_20)

    add_library(barista_drh_server STATIC
        drh/server/interrupt.cpp
        drh/server/server_core.cpp
    )
    add_library(barista::drh_server ALIAS barista_drh_server)
    add_library(drcd_core ALIAS barista_drh_server)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(barista_drh_server PRIVATE drh/server/backend_linux.cpp)
        if(TARGET drc_hostapd)
            add_dependencies(barista_drh_server drc_hostapd)
            target_compile_definitions(barista_drh_server PRIVATE
                DRCD_DEFAULT_HOSTAPD_BIN="${DRC_HOSTAPD_BINARY}")
        endif()
    else()
        target_sources(barista_drh_server PRIVATE drh/server/backend_noop.cpp)
    endif()
    target_include_directories(barista_drh_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(barista_drh_server
        PUBLIC barista_api barista_drh_encoder drc_host drc_ipc
    )
    target_compile_features(barista_drh_server PUBLIC cxx_std_20)

    add_library(drcctl_core STATIC api/legacy_control.cpp)
    target_include_directories(drcctl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(drcctl_core PRIVATE drc_host)
    target_compile_features(drcctl_core PUBLIC cxx_std_20)
else()
    add_library(barista_drh_encoder INTERFACE)
    add_library(barista::drh_encoder ALIAS barista_drh_encoder)
    target_link_libraries(barista_drh_encoder INTERFACE barista_api)
    target_include_directories(barista_drh_encoder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

    add_library(barista_drh_server INTERFACE)
    add_library(barista::drh_server ALIAS barista_drh_server)
    target_link_libraries(barista_drh_server INTERFACE barista_api barista_drh_encoder)
    target_include_directories(barista_drh_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(BUILD_TESTING)
    add_executable(barista_core_tests tests/core_tests.cpp)
    target_link_libraries(barista_core_tests PRIVATE barista_core barista_drh_server barista_drh_encoder)
    add_test(NAME barista_core_tests COMMAND barista_core_tests)

    if(BARISTA_BUILD_ENGINE)
        add_executable(drc_host_tests tests/pairing_tests.cpp)
        target_link_libraries(drc_host_tests PRIVATE drc_host)
        add_test(NAME drc_host_tests COMMAND drc_host_tests)

        add_executable(ap_tsf_clock_tests tests/ap_tsf_clock_tests.cpp)
        target_include_directories(ap_tsf_clock_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(ap_tsf_clock_tests PRIVATE cxx_std_20)
        add_test(NAME ap_tsf_clock_tests COMMAND ap_tsf_clock_tests)

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(drc_ipc_tests tests/app_hook_tests.cpp)
            target_link_libraries(drc_ipc_tests PRIVATE drc_ipc)
            add_test(NAME drc_ipc_tests COMMAND drc_ipc_tests)
        endif()

        add_executable(drcd_encoder_recon_probe tests/encoder_recon_probe.c)
        target_link_libraries(drcd_encoder_recon_probe PRIVATE drc_x264 m pthread dl)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        find_program(DRCD_TEST_FFMPEG ffmpeg)
        if(Python3_Interpreter_FOUND AND DRCD_TEST_FFMPEG)
            add_test(NAME drcd_encoder_reconstruction_tests
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test-encoder-reconstruction.py
                    --probe $<TARGET_FILE:drcd_encoder_recon_probe>
                    --encoder $<TARGET_FILE:drcd_reencode_replay> --ffmpeg ${DRCD_TEST_FFMPEG})
            set_tests_properties(drcd_encoder_reconstruction_tests PROPERTIES TIMEOUT 60)
        endif()

        add_executable(drcd_packet_schedule_tests tests/video_packet_schedule_tests.cpp)
        target_link_libraries(drcd_packet_schedule_tests PRIVATE barista_drh_encoder)
        add_test(NAME drcd_packet_schedule_tests COMMAND drcd_packet_schedule_tests)

        add_executable(drcd_x264_encoder_tests tests/x264_encoder_tests.cpp)
        target_link_libraries(drcd_x264_encoder_tests PRIVATE barista_drh_encoder)
        add_test(NAME drcd_x264_encoder_tests COMMAND drcd_x264_encoder_tests)

        add_executable(drcd_h264_bit_writer_tests tests/h264_bit_writer_tests.cpp)
        target_link_libraries(drcd_h264_bit_writer_tests PRIVATE barista_drh_encoder)
        add_test(NAME drcd_h264_bit_writer_tests COMMAND drcd_h264_bit_writer_tests)

        add_executable(drcd_h264_cabac_tests tests/h264_cabac_tests.cpp)
        target_link_libraries(drcd_h264_cabac_tests PRIVATE barista_drh_encoder)
        add_test(NAME drcd_h264_cabac_tests COMMAND drcd_h264_cabac_tests)

        add_executable(drcd_h264_contexts_tests tests/h264_contexts_tests.cpp)
        target_link_libraries(drcd_h264_contexts_tests PRIVATE barista_drh_encoder)
        add_test(NAME drcd_h264_contexts_tests COMMAND drcd_h264_contexts_tests)

        add_executable(drcd_minih264_tests tests/minih264_tests.cpp)
        target_link_libraries(drcd_minih264_tests PRIVATE barista_drh_encoder)
        add_test(NAME drcd_minih264_tests COMMAND drcd_minih264_tests)

        add_executable(drcd_minih264_cabac_probe tests/minih264_cabac_probe.cpp)
        target_link_libraries(drcd_minih264_cabac_probe PRIVATE barista_drh_encoder)
        if(Python3_Interpreter_FOUND AND DRCD_TEST_FFMPEG)
            add_test(NAME drcd_minih264_cabac_reconstruction
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test-minih264-cabac.py
                    $<TARGET_FILE:drcd_minih264_cabac_probe> ${DRCD_TEST_FFMPEG})
            set_tests_properties(drcd_minih264_cabac_reconstruction PROPERTIES TIMEOUT 60)
            add_test(NAME drcd_minih264_planar_reconstruction
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test-minih264-cabac.py
                    $<TARGET_FILE:drcd_minih264_cabac_probe> ${DRCD_TEST_FFMPEG} planar)
            set_tests_properties(drcd_minih264_planar_reconstruction PROPERTIES TIMEOUT 60)
        endif()

        add_executable(drcd_format_slot_tests tests/format_slot_scheduler_tests.cpp)
        target_link_libraries(drcd_format_slot_tests PRIVATE barista_drh_encoder pthread)
        add_test(NAME drcd_format_slot_tests COMMAND drcd_format_slot_tests)
        set_tests_properties(drcd_format_slot_tests PROPERTIES TIMEOUT 10)

        add_executable(drcd_serial_sender_tests tests/serial_video_sender_tests.cpp)
        target_link_libraries(drcd_serial_sender_tests PRIVATE barista_drh_encoder pthread)
        add_test(NAME drcd_serial_sender_tests COMMAND drcd_serial_sender_tests)
        set_tests_properties(drcd_serial_sender_tests PROPERTIES TIMEOUT 10)

        add_executable(drcd_core_tests tests/server_core_tests.cpp)
        target_link_libraries(drcd_core_tests PRIVATE barista_drh_server)
        add_test(NAME drcd_core_tests COMMAND drcd_core_tests)
        add_test(NAME drcd_fast_encode_tests COMMAND drcd_core_tests)
        set_tests_properties(drcd_fast_encode_tests PROPERTIES ENVIRONMENT "DRCD_FAST_ENCODE=1")
        add_test(NAME drcd_no_intra_refresh_tests COMMAND drcd_core_tests)
        set_tests_properties(drcd_no_intra_refresh_tests PROPERTIES ENVIRONMENT "DRCD_INTRA_REFRESH=0")

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(drcd_real_replay_tests tests/real_replay_tests.cpp)
            target_link_libraries(drcd_real_replay_tests PRIVATE barista_drh_server)

            add_executable(drcd_app_hook_tests tests/app_hook_stream_tests.cpp)
            target_link_libraries(drcd_app_hook_tests PRIVATE barista_drh_server)
            add_test(NAME drcd_app_hook_tests COMMAND drcd_app_hook_tests)
            set_tests_properties(drcd_app_hook_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
            foreach(profile all_idr reference_audio combined)
                add_test(NAME drcd_${profile}_experiment_tests COMMAND drcd_app_hook_tests)
                set_tests_properties(drcd_${profile}_experiment_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
            endforeach()
            set_tests_properties(drcd_all_idr_experiment_tests PROPERTIES ENVIRONMENT "DRCD_ALL_IDR=1")
            set_tests_properties(drcd_reference_audio_experiment_tests PROPERTIES ENVIRONMENT "DRCD_REFERENCE_AUDIO_TIME=1")
            set_tests_properties(drcd_combined_experiment_tests PROPERTIES ENVIRONMENT "DRCD_ALL_IDR=1;DRCD_REFERENCE_AUDIO_TIME=1")
            add_test(NAME drcd_no_intra_refresh_app_hook_tests COMMAND drcd_app_hook_tests)
            set_tests_properties(drcd_no_intra_refresh_app_hook_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
                ENVIRONMENT "DRCD_INTRA_REFRESH=0")
            add_test(NAME drcd_recovery_spacing_tests COMMAND drcd_app_hook_tests)
            set_tests_properties(drcd_recovery_spacing_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
                ENVIRONMENT "DRCD_IDR_PAUSE=1;DRCD_IDR_INIT=1;DRCD_SEND_TIME_VIDEO=1;DRCD_VIDEO_QP=32")
            add_test(NAME drcd_legacy_pacing_tests COMMAND drcd_app_hook_tests)
            set_tests_properties(drcd_legacy_pacing_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
                ENVIRONMENT "DRCD_CHUNK_PACING=0;DRCD_IDR_INIT=0;DRCD_SEND_TIME_VIDEO=0;DRCD_IDR_PAUSE=0;DRCD_VIDEO_QP=32")
        endif()

        add_executable(drcctl_core_tests tests/legacy_control_tests.cpp)
        target_link_libraries(drcctl_core_tests PRIVATE drcctl_core)
        add_test(NAME drcctl_core_tests COMMAND drcctl_core_tests)
    endif()
endif()
