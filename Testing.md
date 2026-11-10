# Testing MuxNexus

MuxNexus is a media tool, so the tests are built around one question: **does
the output contain exactly the streams that went in, and does it still open?**
There are six layers, ordered from fastest to slowest. Each one is listed with
what it covers, how to run it, whether CI runs it, and what it cannot tell you.

| # | Layer | Where | Runs in CI? | Needs |
|---|-------|-------|-------------|-------|
| 1 | Smoke test (CLI end to end) | `scripts/smoke-test.sh` | On demand (`smoke-test.yaml`, manual; Linux + macOS) | `ffmpeg` (with libx264, ideally libvorbis), `cmake`, `ninja`, `pkg-config`, FFmpeg dev libs, C++17 compiler |
| 2 | C++ unit tests (24 cases) | `native/tests/test_checks.cpp` | Yes (`test-native.yaml`: Linux, macOS, FreeBSD) | FFmpeg dev libs, `cmake`, `ninja` |
| 3 | C++ integration tests (6 cases) | `native/tests/test_integration.cpp` | Yes (same workflow) | Fixtures from `generate.sh` (needs the `ffmpeg` program) |
| 3b | C++ merge tests (13 cases: 5 unit, 8 integration) | `native/tests/test_merge.cpp` | Yes (same workflow) | Same fixtures |
| 4 | Fuzzing (2 libFuzzer targets) | `native/fuzz/` | Yes, on PRs and `main` (Linux, clang) | `clang`, FFmpeg dev libs |
| 5 | Kotlin unit tests (25 cases) | `app/src/test/` | Yes (`build.yaml`, on every push; also `test-android.yaml`, manual + weekly) | JDK 21, Android SDK + NDK (Gradle drives it) |
| 6 | Android instrumented tests (3 cases) | `app/src/androidTest/` | **No. The job is switched off (`if: false`).** | Emulator or device, plus a full FFmpeg build |

Static analysis runs alongside these: Android Lint (in `build.yaml`) and
CodeQL (`codeql.yaml`, weekly and on demand).

---

## 1. Smoke test

A few-minute check that the **desktop CLI** builds against your FFmpeg,
remuxes real media, and produces something structurally valid.

```bash
./scripts/smoke-test.sh
```

What it does:

1. Configures and builds only the `muxnexus` CLI into `build-smoke/`.
2. Generates a 1-second H.264 + AAC MP4 with `ffmpeg`.
3. Remuxes it to Matroska, then back to MP4.
4. Compares the codec lists of input and output. They must match.
5. Merges the picture of that MP4 with a FLAC file (`merge ... in.mp4:v lossless.flac:best`)
   and requires H.264 + FLAC and no AAC in the result. Also checks that
   `probe` names the FLAC as the best audio.
6. Tries Vorbis-in-Matroska to MP4. Depending on the FFmpeg version this is
   either rejected or allowed with a warning; **both outcomes pass** (see
   "FFmpeg version differences" below).

Set `SMOKE_ARTIFACT_DIR=/some/dir` and the script keeps its input, output and
codec reports there instead of deleting them. CI does this and uploads the
folder as `smoke-test-artifacts-<os>`, even when a step fails.

## 2. C++ unit tests

Pure-function tests with [doctest](https://github.com/doctest/doctest). No
media files needed.

```bash
cmake -S native -B build -G Ninja -DMUXNEXUS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure -R unit
```

Covers:

- **Container fingerprinting** from magic bytes: MP4, MOV, Matroska, MPEG-TS,
  FLV, Ogg, AVI, WAV, an empty file, and random bytes.
- **The codec/container compatibility matrix**: for example H.264 in MP4
  (allowed), HEVC in MP4 (allowed with a note), Vorbis in MP4 (warning, with a
  "use Matroska" suggestion), Theora/TrueHD/text subtitles in MP4 (rejected).
- **The resume-hash function** (`Fnv1aFdHeadTail`): deterministic for the same
  bytes, different when a byte or the size differs.

Note: the fingerprinter needs at least 12 bytes of input. Tests that feed it a
container header must pad to that length, as real files always are.

## 3. C++ integration tests

These run the real `RunRemux` pipeline on real files.

```bash
bash ./native/tests/fixtures/generate.sh      # needs the ffmpeg program
ctest --test-dir build --output-on-failure -R integration
```

`generate.sh` writes into `native/tests/fixtures/generated/` (git-ignored):
`aac.mp4` (H.264 + AAC), `aac.mkv`, `vorbis.mkv` (only if your `ffmpeg` has
libvorbis), a 1 KiB `truncated.mp4`, a zero-byte `empty.mp4`, and the merge
fixtures: `merge_video.mp4` (video only), `swap_b.mkv` (a second video with
different audio), `lossless.flac` and `lossless_long.flac` (24-bit/48 kHz),
and `multi_audio.mkv` (video + AAC, FLAC and AC-3 tracks). Point the
tests somewhere else with `MUXNEXUS_FIXTURES_DIR`. If fixtures are missing the
tests skip (exit code 77) rather than fail.

Cases:

1. MP4 to MKV succeeds **and the output is verified**.
2. MP4 to MP4 round trip preserves the streams.
3. Vorbis MKV to MP4: an error on newer FFmpeg, a success with a warning on
   FFmpeg 6.1; either way the user is pointed at Matroska.
4. Vorbis MKV to MKV succeeds.
5. An empty file fails cleanly (no crash, no hang).
6. Cancelling returns `CANCELLED` with a valid checkpoint.

One detail worth knowing when you write new tests: output verification
re-reads the output file through a duplicate of the output descriptor, so the
output must be opened **read/write** (`O_RDWR`), not write-only. A write-only
descriptor silently disables verification.

### 3b. Merge tests

`test_merge` covers the stream-merging engine (`native/core/merge_core.cpp`).
Run with `ctest --test-dir build --output-on-failure -R merge`.

- **Audio ranking** (no files needed): lossless beats any lossy bit rate, higher
  bit-depth x sample-rate wins among lossless, higher bit rate wins among lossy,
  ties go to the earlier stream, non-audio streams are ignored.
- **Probe** lists a three-audio-track file and picks the FLAC track as best.
- **Audio swap**: picture of one video + audio of another. The test hashes every
  packet's payload and requires the output's video hash to equal video A's and
  its audio hash to equal video B's. That is the "no re-encoding" claim, checked
  bit for bit.
- **Best of several tracks**: a file with AAC 64k, FLAC and AC-3 192k keeps only
  the FLAC.
- **Lossless file + video** into MKV, hash-checked.
- **Trimming**: a 4-second audio file over a 2-second video gives a ~2-second
  result, and the user is told it was trimmed.
- **Delay**: `offset_us = 500000` moves the audio start to about 0.5 s.
- **MP4 output** with AAC from a second file, verified.
- **Bad requests** (no inputs, stream index out of range, asking for audio from
  a file that has none) fail with an error instead of writing a broken file.

Not covered: audio that is *shorter* than the video (the "rest will be silent"
warning), merging three or more inputs, subtitle streams, and sync between
unrelated recordings.

## 4. Fuzzing

libFuzzer targets for the two places where untrusted bytes arrive:

- `fuzz_fingerprint`: the magic-byte container sniffer.
- `fuzz_demux`: FFmpeg's demuxer fed from memory through our custom I/O.

```bash
cmake -S native -B build-fuzz -G Ninja -DMUXNEXUS_BUILD_FUZZ=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz
mkdir -p corpus
./build-fuzz/fuzz/fuzz_demux -max_total_time=60 corpus/
```

Built with AddressSanitizer and UndefinedBehaviorSanitizer. Requires clang
(the CMake file skips the targets with a warning on other compilers). CI runs
30 seconds of `fuzz_fingerprint` and 60 seconds of `fuzz_demux` on pull
requests and on `main`. A short run finds shallow bugs
only; it is not a substitute for a long fuzzing campaign.

## 5. Kotlin unit tests

```bash
./gradlew :app:testDebugUnitTest
```

25 tests, run on the JVM with Robolectric (Android API 36 sandbox):

- `BatchQueueTest` (18): enqueue/order, remove, move up/down (including edge
  cases), status transitions, resetting stale `RUNNING` jobs, persistence across
  instances, and recovery from corrupt stored JSON.
- `OutputFormatTest` (7): the MP4/MOV/MKV definitions are consistent
  (muxer names, extensions, MIME types).

These do **not** load the native library or touch FFmpeg.

## 6. Android instrumented tests (currently disabled in CI)

```bash
./gradlew :app:connectedDebugAndroidTest
```

Needs an emulator or device and the Android FFmpeg libraries
(`./native-build/build_ffmpeg.sh android`). The three tests launch the
activity and check the title, the empty-queue message, and that "Add your first
file" opens the output-format dialog.

The `instrumented` job in `test-android.yaml` is set to `if: false`, so **CI
does not run these**. They are real tests you can run locally; they are just
not enforced yet.

---

## What CI runs, workflow by workflow

| Workflow | Triggers | What it checks |
|----------|----------|----------------|
| `build.yaml` | every push to any branch (docs-only pushes skipped); manual; called by `release.yaml` | Android Lint, Kotlin unit tests, and all four APKs. With `build_cli` on, also the CLI binaries (Linux x86_64 and arm64, macOS arm64), each self-tested: MP4/MOV/MKV output, one input per extra format (TS, FLV, AVI, FLAC, WAV, Ogg, AC-3, MP3, raw AAC), and a merge |
| `test-native.yaml` | push to `main` / pull requests touching `native/**`; manual | C++ unit, merge and integration tests on Ubuntu, macOS and FreeBSD; fuzz build and short run |
| `test-android.yaml` | manual; weekly (Mon 05:00 UTC) | Kotlin unit tests; (instrumented tests disabled) |
| `smoke-test.yaml` | manual | CLI smoke test on Ubuntu and macOS, with artifacts |
| `codeql.yaml` | manual; weekly (Mon 06:00 UTC) | CodeQL static analysis (Java/Kotlin and C++) |
| `release.yaml` | manual (you enter the version) | Validates the version, runs `build.yaml` with the CLI builds on, attests the files (public repo only) and creates the GitHub Release |
| `dependency-submission.yaml` | push to `main`, manual | Feeds the Gradle dependency graph to GitHub (Dependabot alerts) |
| `nightly.yaml` | daily 04:00 UTC, manual | Rebuilds the Android FFmpeg libraries |

## FFmpeg version differences

CI deliberately runs against different FFmpeg versions: Ubuntu ships 6.1,
Homebrew ships 9.x, FreeBSD ships its own. They disagree on edge cases. The
known one: FFmpeg 6.1's MP4 muxer accepts Vorbis audio, while newer versions
reject it. MuxNexus only *warns* for that combination, so tests assert the
warning and Matroska suggestion rather than a hard failure. If you add a test
that depends on muxer behaviour, check it on both a 6.x and a 9.x FFmpeg.

The code branches on `LIBAVFORMAT_VERSION_MAJOR >= 61` for an API signature
change (`const` write buffers). Only the pre-61 branch is exercised by the
pinned Android FFmpeg (n6.1.6); the other is covered by macOS/Homebrew builds.

## Troubleshooting

- **`Permission denied` running `generate.sh`**: the executable bit was lost
  (common after uploading through the GitHub web UI). Run it as
  `bash ./native/tests/fixtures/generate.sh`.
- **`fatal error: libavcodec/...: file not found` on macOS**: use Homebrew's
  FFmpeg and make sure `pkg-config` can see it (`pkg-config --cflags libavcodec`).
- **Linker errors for `LogInfo` / `LogError`**: link `muxnexus_core` through its
  CMake target (it brings `muxnexus_desktop_log` with it). Android provides
  these two functions in `jni_bridge.cpp` instead.
- **Integration tests report "skipped"**: fixtures are missing. Run
  `generate.sh`, and check that your `ffmpeg` has `libx264`.
- **Vorbis cases missing**: your `ffmpeg` has no `libvorbis`; the fixture and
  the dependent checks are skipped.

## What is not tested yet

Being straight about the gaps:

- **Real devices.** Nothing runs on physical Android hardware in CI. The
  instrumented tests are disabled, and the JNI bridge, Storage Access
  Framework file handling and the native library loading are only covered by
  manual use.
- **Output verification inside the app.** The app currently opens the output
  file write-only, so read-back verification probably never runs there (see the
  README's known issues). The desktop CLI and the C++ tests do verify.
- **Concurrent / killed-process behaviour** of the job queue (persisting
  mid-job and then killing the app).
- **Storage Access Framework permission expiry** (some devices revoke
  persisted URI grants after a while).
- **Resume from a checkpoint.** The engine can produce a checkpoint, but no
  resume flow exists to test.
- **16 KB page-size alignment** on Android 15+ devices. The link flag is set;
  it has not been checked on a device.
- **Large files** (multi-gigabyte inputs, the 4-hour job limit, the
  >2 GiB verification skip).
- **A broad media corpus.** Fixtures are small synthetic files generated with
  FFmpeg. Real-world files from phones, cameras and encoders are where odd
  edge cases live.

These are good first issues, not things to hide.

## Adding tests

- A new codec/container rule: add a `TEST_CASE` to `test_checks.cpp`, next to
  the existing "Codec matrix" cases.
- A new end-to-end scenario: add the fixture to `generate.sh`, then a
  `TEST_CASE` in `test_integration.cpp` using `require_fixture("name")`.
- A parser crash you found by hand: add its bytes to a fuzz corpus and a unit
  test that pins the fix.