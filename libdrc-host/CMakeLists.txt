add_library(drc_host STATIC
    src/mac_address.cpp
    src/pairing.cpp
    src/runtime_transport.cpp
    src/session.cpp
    src/version.cpp
)

target_include_directories(drc_host
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(drc_host PUBLIC cxx_std_20)

option(DRC_HOST_BUILD_TESTS "Build libdrc-host unit tests" ON)
if(DRC_HOST_BUILD_TESTS)
    add_executable(drc_host_tests
        tests/pairing_tests.cpp
    )
    target_link_libraries(drc_host_tests PRIVATE drc_host)
    target_compile_features(drc_host_tests PRIVATE cxx_std_20)
    add_test(NAME drc_host_tests COMMAND drc_host_tests)
    add_executable(ap_tsf_clock_tests tests/ap_tsf_clock_tests.cpp)
    target_compile_features(ap_tsf_clock_tests PRIVATE cxx_std_20)
    add_test(NAME ap_tsf_clock_tests COMMAND ap_tsf_clock_tests)
endif()
