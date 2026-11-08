# Testing MuxNexus

Four tiers, run in order from fastest to slowest.

## 1. Smoke (`scripts/smoke-test.sh`)

Five-minute sanity check. Verifies the desktop CLI builds, opens a real
container, remuxes it, and the output is structurally valid.

    ./scripts/smoke-test.sh

Requires: `ffmpeg`, `cmake`, `ninja`, C++17 compiler.

## 2. C++ unit tests (`native/tests`)

Pure functions only. Runs in milliseconds.

    cmake -S native -B build-native-test -DMUXNEXUS_BUILD_TESTS=ON
    cmake --build build-native-test
    ctest --test-dir build-native-test --output-on-failure

Covers: `FingerprintFd`, `CheckCodecContainer`, `Fnv1aFdHeadTail`.

## 3. C++ integration tests (`native/tests/fixtures`)

Generates real media fixtures with `ffmpeg`, then runs the full
`RunRemux` pipeline.

    ./native/tests/fixtures/generate.sh
    ctest --test-dir build-native-test -R integration --output-on-failure

Skips cleanly if `ffmpeg` is not on `PATH`.

## 4. Fuzz tests (`native/fuzz`)

libFuzzer targets for the two entry points where user bytes arrive.

    cmake -S native -B build-fuzz -DMUXNEXUS_BUILD_FUZZ=ON \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-fuzz
    ./build-fuzz/fuzz/fuzz_demux -max_total_time=60 corpus/

## 5. Android unit tests

    ./gradlew :app:testDebugUnitTest

Covers: `BatchQueue` (enqueue/remove/reorder/persist/corrupt-JSON recovery),
`OutputFormat` consistency. Uses Robolectric.

## 6. Android instrumented tests

    ./gradlew :app:connectedDebugAndroidTest

Requires: emulator or device, full FFmpeg build.
Covers: activity launch, Compose UI smoke.

## CI

`.github/workflows/test-native.yml` runs the C++ suite on
ubuntu-latest / macos-latest / FreeBSD (via vmactions).
`.github/workflows/test-android.yml` runs Kotlin unit tests.

## What is NOT tested yet

- Concurrent job persistence under process kill
- SAF URI permission expiry (some devices revoke after 30 days)
- Resume from checkpoint end-to-end in the Android UI
- FFmpeg version-specific differences beyond `LIBAVFORMAT_VERSION_MAJOR`
- 16 KB page alignment on Android 15+ (needs a real device check)

Track these as issues. They are roadmap credibility, not gaps to hide.