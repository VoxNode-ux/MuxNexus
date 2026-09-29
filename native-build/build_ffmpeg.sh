#!/usr/bin/env bash
# Run this yourself, once per machine, from a shell that has:
#   - network access (to git clone FFmpeg)
#   - the Android NDK installed (set ANDROID_NDK_HOME below)
# It is NOT run by Gradle and NOT run by Claude — cross-compiling FFmpeg
# takes real wall-clock time (10-30+ min per ABI) and needs a live toolchain,
# neither of which is available inside this chat's sandbox.
#
# Usage:
#   export ANDROID_NDK_HOME=/path/to/Android/sdk/ndk/<version>
#   ./build_ffmpeg.sh
#
# Output lands in app/src/main/cpp/ffmpeg/{include,<ABI>/lib}, matching what
# CMakeLists.txt expects.

set -euo pipefail

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your NDK install first}"

FFMPEG_VERSION="n6.1.1"
OUT_ROOT="$(cd "$(dirname "$0")/.." && pwd)/app/src/main/cpp/ffmpeg"
API_LEVEL=26

ABIS=(
  "arm64-v8a:aarch64-linux-android:arm64"
  "armeabi-v7a:armv7a-linux-androideabi:arm"
)

if [ ! -d ffmpeg-src ]; then
  git clone --depth 1 --branch "${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
fi

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"

for entry in "${ABIS[@]}"; do
  IFS=":" read -r abi triple arch <<< "$entry"
  echo "=== Building FFmpeg for ${abi} ==="

  BUILD_DIR="build-${abi}"
  rm -rf "${BUILD_DIR}"
  mkdir -p "${BUILD_DIR}"

  pushd ffmpeg-src >/dev/null
  make distclean >/dev/null 2>&1 || true

  ./configure \
    --prefix="$(pwd)/../${BUILD_DIR}" \
    --target-os=android \
    --arch="${arch}" \
    --cpu=generic \
    --cc="${TOOLCHAIN}/bin/${triple}${API_LEVEL}-clang" \
    --cxx="${TOOLCHAIN}/bin/${triple}${API_LEVEL}-clang++" \
    --sysroot="${TOOLCHAIN}/sysroot" \
    --enable-cross-compile \
    --disable-shared \
    --enable-static \
    --enable-pic \
    --disable-doc \
    --disable-programs \
    --disable-avdevice \
    --disable-postproc \
    --disable-avfilter \
    --disable-network \
    --disable-protocols \
    --enable-protocol=file \
    --disable-encoders \
    --disable-muxers \
    --enable-muxer=mp4 \
    --enable-muxer=mov \
    --enable-muxer=matroska \
    --disable-decoders \
    --disable-bsfs \
    --enable-bsf=aac_adtstoasc \
    --disable-demuxers \
    --enable-demuxer=mov \
    --enable-demuxer=matroska \
    --enable-demuxer=aac \
    --disable-indevs \
    --disable-outdevs

  make -j"$(nproc)"
  make install
  popd >/dev/null

  mkdir -p "${OUT_ROOT}/${abi}/lib"
  cp "${BUILD_DIR}"/lib/lib*.a "${OUT_ROOT}/${abi}/lib/"
done

mkdir -p "${OUT_ROOT}/include"
cp -r "build-arm64-v8a/include/"* "${OUT_ROOT}/include/"

echo "Done. FFmpeg static libs are in ${OUT_ROOT}/<abi>/lib/"
echo "--disable-network + --disable-protocols with only 'file' enabled means"
echo "no socket code is even compiled in, matching the compile-time socket"
echo "stripping requirement (not just a runtime permission omission)."
 
