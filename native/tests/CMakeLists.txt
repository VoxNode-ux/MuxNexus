include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(doctest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

add_executable(test_checks test_checks.cpp)
target_link_libraries(test_checks PRIVATE muxnexus_core doctest::doctest PkgConfig::FFMPEG)
target_include_directories(test_checks PRIVATE ${CMAKE_SOURCE_DIR}/core)
add_test(NAME unit COMMAND test_checks)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE muxnexus_core doctest::doctest PkgConfig::FFMPEG)
target_include_directories(test_integration PRIVATE ${CMAKE_SOURCE_DIR}/core)
add_test(NAME integration COMMAND test_integration)
set_tests_properties(integration PROPERTIES
    ENVIRONMENT "MUXNEXUS_FIXTURES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generated"
    SKIP_RETURN_CODE 77
)