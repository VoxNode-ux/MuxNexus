cmake_minimum_required(VERSION 3.22.1)
project(remux_engine)

# Expects FFmpeg to already be cross-compiled per ABI and dropped here by
# native-build/build_ffmpeg.sh, i.e.:
#   app/src/main/cpp/ffmpeg/include/           (shared headers)
#   app/src/main/cpp/ffmpeg/<ABI>/lib/*.a       (per-ABI static libs)
set(FFMPEG_ROOT ${CMAKE_SOURCE_DIR}/ffmpeg)
set(FFMPEG_LIB_DIR ${FFMPEG_ROOT}/${ANDROID_ABI}/lib)
set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT}/include)

add_library(remux_engine SHARED
    remux_engine.cpp
)

target_include_directories(remux_engine PRIVATE
    ${FFMPEG_INCLUDE_DIR}
)

# Static-link FFmpeg's pieces so there is exactly one .so to ship, and so the
# --disable-network build config can't be reintroduced by a stray dynamic
# dependency at runtime.
add_library(avformat STATIC IMPORTED)
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavformat.a)

add_library(avcodec STATIC IMPORTED)
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavcodec.a)

add_library(avutil STATIC IMPORTED)
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavutil.a)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(remux_engine
    avformat
    avcodec
    avutil
    ${log-lib}
    ${android-lib}
)

target_compile_options(remux_engine PRIVATE -O2 -fvisibility=hidden)
 
