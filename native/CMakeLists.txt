cmake_minimum_required(VERSION 3.22)
project(muxnexus_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(muxnexus_core STATIC
    core/remux_core.cpp
    core/remux_checks.cpp
)
target_include_directories(muxnexus_core PUBLIC core)

if(ANDROID)
    set(FFMPEG_ROOT ${CMAKE_SOURCE_DIR}/../app/src/main/cpp/ffmpeg)
    set(FFMPEG_LIB_DIR ${FFMPEG_ROOT}/${ANDROID_ABI}/lib)
    set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT}/include)
    # muxnexus_core itself includes FFmpeg headers (remux_core.cpp, remux_checks.cpp)
    target_include_directories(muxnexus_core PUBLIC ${FFMPEG_INCLUDE_DIR})

    add_library(avformat STATIC IMPORTED)
    set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavformat.a)
    add_library(avcodec STATIC IMPORTED)
    set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavcodec.a)
    add_library(avutil STATIC IMPORTED)
    set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavutil.a)

    add_library(remux_engine SHARED android/jni_bridge.cpp)
    target_link_libraries(remux_engine muxnexus_core avformat avcodec avutil log android)
    target_include_directories(remux_engine PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_compile_options(remux_engine PRIVATE -O2 -fvisibility=hidden)
    # 16 KB page size for Android 15+ (targetSdk 35+)
    target_link_options(remux_engine PRIVATE -Wl,-z,max-page-size=16384)

else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
    # The core library includes FFmpeg headers itself, so it needs the include
    # paths too (macOS/Homebrew keeps them outside the default search path).
    target_link_libraries(muxnexus_core PUBLIC PkgConfig::FFMPEG)

    # Log sinks that core calls (LogInfo/LogError); Android defines its own.
    add_library(muxnexus_desktop_log STATIC desktop/log_desktop.cpp)
    target_link_libraries(muxnexus_core INTERFACE muxnexus_desktop_log)

    add_executable(muxnexus desktop/main_desktop.cpp)
    target_link_libraries(muxnexus muxnexus_core PkgConfig::FFMPEG)

    if(WIN32)
        target_link_libraries(muxnexus ws2_32)
    endif()
endif()

if(MUXNEXUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MUXNEXUS_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()