add_library(drcctl_core STATIC
    src/client.cpp
)

target_include_directories(drcctl_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(drcctl_core
    PRIVATE
        drc_host
)

target_compile_features(drcctl_core PUBLIC cxx_std_20)

add_executable(drcctl
    src/main.cpp
)

target_link_libraries(drcctl
    PRIVATE
        drcctl_core
        drc_host
)

target_compile_features(drcctl PRIVATE cxx_std_20)

option(DRCCTL_BUILD_TESTS "Build drcctl unit tests" ON)
if(DRCCTL_BUILD_TESTS)
    add_executable(drcctl_core_tests
        tests/client_tests.cpp
    )
    target_link_libraries(drcctl_core_tests PRIVATE drcctl_core)
    target_compile_features(drcctl_core_tests PRIVATE cxx_std_20)
    add_test(NAME drcctl_core_tests COMMAND drcctl_core_tests)
endif()
