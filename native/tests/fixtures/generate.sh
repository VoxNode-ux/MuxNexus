#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

OUT="generated"
mkdir -p "$OUT"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg not found — skipping fixture generation." >&2
    exit 0
fi

FF="ffmpeg -hide_banner -loglevel error -y"

echo "Generating aac.mp4 ..."
$FF -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -c:a aac -b:a 64k -shortest "$OUT/aac.mp4"

echo "Generating aac.mkv ..."
$FF -i "$OUT/aac.mp4" -c copy "$OUT/aac.mkv"

if $FF -hide_banner -codecs 2>/dev/null | grep -q libvorbis; then
    echo "Generating vorbis.mkv ..."
    $FF -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" \
        -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
        -c:a libvorbis -q:a 3 -shortest "$OUT/vorbis.mkv"
else
    echo "libvorbis not available — skipping vorbis.mkv" >&2
fi

echo "Generating merge fixtures ..."
# Video only (picture "A"), 2 s.
$FF -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    "$OUT/merge_video.mp4"
# A second, different video with its own AAC audio ("B": 880 Hz).
$FF -f lavfi -i "testsrc2=size=320x240:rate=30:duration=2" \
    -f lavfi -i "sine=frequency=880:sample_rate=44100:duration=2" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -c:a aac -b:a 64k -shortest "$OUT/swap_b.mkv"
# Lossless 24-bit/48 kHz audio file, 2 s and a longer 4 s one.
$FF -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=2" \
    -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "$OUT/lossless.flac"
$FF -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=4" \
    -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "$OUT/lossless_long.flac"
# One video with three audio tracks: AAC 64k, FLAC, AC-3 192k.
$FF -f lavfi -i "testsrc=size=320x240:rate=30:duration=2" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
    -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=2" \
    -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=2" \
    -map 0:v -map 1:a -map 2:a -map 3:a \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -c:a:0 aac -b:a:0 64k -c:a:1 flac -c:a:2 ac3 -b:a:2 192k \
    -shortest "$OUT/multi_audio.mkv"

echo "Generating truncated.mp4 ..."
[ -f "$OUT/aac.mp4" ] && head -c 1024 "$OUT/aac.mp4" > "$OUT/truncated.mp4"

echo "Generating empty.mp4 ..."
: > "$OUT/empty.mp4"

echo "Done. Fixtures in $(pwd)/$OUT"
ls -la "$OUT"