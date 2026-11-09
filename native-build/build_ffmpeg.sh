#!/usr/bin/env bash
# Build FFmpeg for Android (all ABIs) or the host (Linux/macOS/FreeBSD).
# Usage:
#   ./build_ffmpeg.sh android      # arm64-v8a, armeabi-v7a, x86_64
#   ./build_ffmpeg.sh desktop      # host arch, static libs for the CLI (build-desktop/)
set -euo pipefail
cd "$(dirname "$0")"
cd ..

TARGET="${1:-android}"
FFMPEG_VERSION="n6.1.6"
FFMPEG_COMMIT="f1e3a2bf7a2f2cde936d1ed97f09a26853d20125"  # commit for n6.1.6
OUT_ROOT="$(pwd)/app/src/main/cpp/ffmpeg"
API_LEVEL=29

ABIS=(
  "arm64-v8a:aarch64-linux-android:arm64"
  "armeabi-v7a:armv7a-linux-androideabi:arm"
  "x86_64:x86_64-linux-android:x86_64"
)

clone_ffmpeg() {
  if [ ! -d ffmpeg-src ]; then
    git clone --depth 1 --branch "${FFMPEG_VERSION}" \
      https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
  fi
  actual="$(git -C ffmpeg-src rev-parse HEAD)"
  if [ "${actual}" != "${FFMPEG_COMMIT}" ]; then
    echo "FFmpeg commit mismatch: expected ${FFMPEG_COMMIT}, got ${actual}" >&2
    exit 1
  fi
}

# Input formats. Output stays mp4/mov/matroska. Every demuxer is parser code
# that reads untrusted files, so this list is deliberately short.
#   containers : mov (mp4/m4a/m4v), matroska (mkv/webm), mpegts, mpegps, avi,
#                flv, ogg, asf (wmv/wma)
#   audio files: mp3, wav, aiff, flac, ac3, eac3, dts, dtshd, truehd, ape, wv,
#                caf, aac (raw ADTS)
#   raw video  : h264, hevc
DEMUXERS=(mov matroska aac mpegts mpegps avi flv ogg asf mp3 wav aiff flac
          ac3 eac3 dts dtshd truehd ape wv caf h264 hevc)
DEMUXER_FLAGS=()
for d in "${DEMUXERS[@]}"; do DEMUXER_FLAGS+=("--enable-demuxer=${d}"); done

common_configure() {
  ./configure \
    --disable-shared --enable-static --enable-pic \
    --disable-autodetect --disable-doc --disable-programs \
    --disable-avdevice --disable-postproc --disable-avfilter \
    --disable-network --disable-protocols --enable-protocol=file \
    --disable-encoders --disable-muxers \
    --enable-muxer=mp4 --enable-muxer=mov --enable-muxer=matroska \
    --disable-decoders --disable-bsfs \
    --enable-bsf=aac_adtstoasc --enable-bsf=hevc_mp4toannexb \
    --enable-bsf=extract_extradata \
    --disable-demuxers \
    "${DEMUXER_FLAGS[@]}" \
    --disable-indevs --disable-outdevs "$@"
}

build_android() {
  : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME first}"
  HOST_TAG="linux-x86_64"
  case "$(uname -s)" in
    Darwin) HOST_TAG="darwin-x86_64" ;;
    MINGW*|MSYS*) HOST_TAG="windows-x86_64" ;;
  esac
  TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
  clone_ffmpeg

  for entry in "${ABIS[@]}"; do
    IFS=":" read -r abi triple arch <<< "$entry"
    echo "=== Android ${abi} ==="
    rm -rf "build-${abi}"; mkdir -p "build-${abi}"
    pushd ffmpeg-src >/dev/null
    make distclean >/dev/null 2>&1 || true

    common_configure \
      --prefix="$(pwd)/../build-${abi}" \
      --target-os=android --arch="${arch}" --cpu=generic \
      --cc="${TOOLCHAIN}/bin/${triple}${API_LEVEL}-clang" \
      --cxx="${TOOLCHAIN}/bin/${triple}${API_LEVEL}-clang++" \
      --strip="${TOOLCHAIN}/bin/llvm-strip" \
      --sysroot="${TOOLCHAIN}/sysroot" \
      --enable-cross-compile --disable-x86asm

    make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
    make install
    popd >/dev/null

    mkdir -p "${OUT_ROOT}/${abi}/lib"
    cp "build-${abi}"/lib/lib*.a "${OUT_ROOT}/${abi}/lib/"
  done
  mkdir -p "${OUT_ROOT}/include"
  cp -r build-arm64-v8a/include/* "${OUT_ROOT}/include/"
  echo "Android FFmpeg done: ${OUT_ROOT}"
}

build_desktop() {
  clone_ffmpeg
  pushd ffmpeg-src >/dev/null
  make distclean >/dev/null 2>&1 || true
  # --disable-x86asm: no nasm needed on CI; irrelevant for stream copying.
  common_configure --prefix="$(pwd)/../build-desktop" --disable-x86asm
  make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
  make install
  popd >/dev/null
  echo "Desktop FFmpeg done: build-desktop/"
}

case "$TARGET" in
  android) build_android ;;
  desktop) build_desktop ;;
  *) echo "Usage: $0 [android|desktop]"; exit 1 ;;
esac