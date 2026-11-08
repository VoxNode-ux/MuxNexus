if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Fuzz targets require clang; skipping.")
    return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

foreach(fuzz_target fuzz_fingerprint fuzz_demux)
    add_executable(${fuzz_target} ${fuzz_target}.cpp)
    target_link_libraries(${fuzz_target} PRIVATE muxnexus_core PkgConfig::FFMPEG)
    target_include_directories(${fuzz_target} PRIVATE ${CMAKE_SOURCE_DIR}/core)
    target_compile_options(${fuzz_target} PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined -g -O1 -fno-omit-frame-pointer)
    target_link_options(${fuzz_target} PRIVATE
        -fsanitize=fuzzer,address,undefined)
endforeach()
