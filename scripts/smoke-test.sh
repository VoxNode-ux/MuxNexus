#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> Checking prerequisites"
for cmd in cmake ffmpeg pkg-config; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "missing: $cmd" >&2; exit 1; }
done

BUILD_DIR="build-smoke"
rm -rf "$BUILD_DIR"

echo "==> Configuring"
cmake -S native -B "$BUILD_DIR" -G Ninja -DMUXNEXUS_BUILD_TESTS=OFF

echo "==> Building CLI"
cmake --build "$BUILD_DIR" --target muxnexus

echo "==> Generating test file"
TMP_DIR="$(mktemp -d)"

# When SMOKE_ARTIFACT_DIR is set (CI), keep the inputs, outputs and codec
# reports there so the workflow can upload them, even when the test fails.
collect_artifacts() {
    if [ -n "${SMOKE_ARTIFACT_DIR:-}" ]; then
        mkdir -p "$SMOKE_ARTIFACT_DIR"
        cp -f "$TMP_DIR"/* "$SMOKE_ARTIFACT_DIR"/ 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap collect_artifacts EXIT
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc=size=320x240:rate=30:duration=3" \
    -f lavfi -i "sine=frequency=440:duration=3" \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
    -c:a aac -b:a 64k -shortest "$TMP_DIR/in.mp4"

echo "==> Remuxing MP4 -> MKV"
"$BUILD_DIR/muxnexus" "$TMP_DIR/in.mp4" "$TMP_DIR/out.mkv" matroska

echo "==> Verifying output"
[ -s "$TMP_DIR/out.mkv" ] || { echo "FAIL: empty output" >&2; exit 1; }
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name -of csv \
    "$TMP_DIR/out.mkv" | sort > "$TMP_DIR/codecs_out"
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name -of csv \
    "$TMP_DIR/in.mp4" | sort > "$TMP_DIR/codecs_in"
diff -q "$TMP_DIR/codecs_in" "$TMP_DIR/codecs_out" >/dev/null || {
    echo "FAIL: codec set changed" >&2; exit 1
}

echo "==> Remuxing Vorbis MKV -> MP4 (expect failure)"
if ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc=size=320x240:rate=30:duration=1" \
    -f lavfi -i "sine=frequency=440:duration=1" \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
    -c:a libvorbis -shortest "$TMP_DIR/vorbis.mkv" 2>/dev/null; then
    if "$BUILD_DIR/muxnexus" "$TMP_DIR/vorbis.mkv" "$TMP_DIR/vorbis.mp4" mp4; then
        echo "FAIL: Vorbis -> MP4 should have failed" >&2; exit 1
    else
        echo "PASS: Vorbis -> MP4 rejected as expected"
    fi
else
    echo "SKIP: libvorbis unavailable"
fi

echo
echo "=========================================="
echo "SMOKE TEST PASSED"
echo "=========================================="