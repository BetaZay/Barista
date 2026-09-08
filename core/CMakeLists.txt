add_library(barista_api INTERFACE)
add_library(barista::api ALIAS barista_api)
target_include_directories(barista_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(barista_api INTERFACE cxx_std_20)

add_library(barista_drh_server INTERFACE)
add_library(barista::drh_server ALIAS barista_drh_server)
target_link_libraries(barista_drh_server INTERFACE barista_api)
target_include_directories(barista_drh_server INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(barista_drh_server INTERFACE cxx_std_20)

add_library(barista_drh_encoder INTERFACE)
add_library(barista::drh_encoder ALIAS barista_drh_encoder)
target_link_libraries(barista_drh_encoder INTERFACE barista_api)
target_include_directories(barista_drh_encoder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(barista_drh_encoder INTERFACE cxx_std_20)

# Compatibility target for existing desktop callers. New code should depend on
# the narrow barista_api and barista_drh_* targets instead.
add_library(barista_core INTERFACE)
target_include_directories(barista_core INTERFACE include)
target_link_libraries(barista_core INTERFACE barista_api)
target_compile_features(barista_core INTERFACE cxx_std_20)
if(BUILD_TESTING)
    add_executable(barista_core_tests tests/core_tests.cpp)
    target_link_libraries(barista_core_tests PRIVATE barista_core barista_drh_server barista_drh_encoder)
    add_test(NAME barista_core_tests COMMAND barista_core_tests)
endif()
