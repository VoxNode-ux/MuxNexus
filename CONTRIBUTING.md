# Contributing to MuxNexus

Thanks for considering a contribution. This is a small, focused FOSS project —
please keep PRs scoped and avoid drive-by refactors mixed in with feature work.

## Before you start

- Open an issue first for anything beyond a trivial fix, so design direction
  can be agreed before you invest time.
- The native engine lives in `native/`. The Android app needs FFmpeg built
  locally first; see "Build from source" in `README.md`. The desktop CLI and
  the C++ tests only need your system FFmpeg and CMake.

## Development setup

1. Clone the repo.
2. For the Android app: run `native-build/build_ffmpeg.sh android` once (see
   README for prerequisites), then `./gradlew :app:assembleDebug` (or open the
   project in Android Studio).
3. For the C++ engine and CLI: follow "Testing" in `Testing.md`; no Android
   tooling is needed.

## Pull requests

- One logical change per PR.
- Match existing Kotlin style (no custom formatter is enforced yet — keep it
  consistent with surrounding code).
- Native (C++) changes should note which platform(s) and FFmpeg version(s) you
  tested on, and pass the C++ tests (`Testing.md`). If you touch Android-only
  code, note which ABI(s) you tested on.
- Update `README.md` / `Testing.md` if you change the build steps, add a
  dependency, or add a test layer.
- Pin any new GitHub Action to a full commit SHA (with the version in a comment).

## Reporting bugs

Use the bug report issue template. Include the input file's container/codec
if the bug is remux-specific (e.g. "MKV, H.265 + Opus") — that's almost
always the first thing needed to reproduce.

## Security issues

Do not open a public issue — see `SECURITY.md`.