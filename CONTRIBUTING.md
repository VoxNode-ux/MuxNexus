# Contributing to MuxNexus

Thanks for considering a contribution. This is a small, focused FOSS project —
please keep PRs scoped and avoid drive-by refactors mixed in with feature work.

## Before you start

- Open an issue first for anything beyond a trivial fix, so design direction
  can be agreed before you invest time.
- The native engine (`app/src/main/cpp/`) requires building FFmpeg locally —
  see the root `README.md` build sequence before you can compile the app.

## Development setup

1. Clone the repo.
2. Run `native-build/build_ffmpeg.sh` once (see README for prerequisites).
3. Open in Android Studio; let Gradle sync.

## Pull requests

- One logical change per PR.
- Match existing Kotlin style (no custom formatter is enforced yet — keep it
  consistent with surrounding code).
- Native (C++) changes should note which ABI(s) you tested on.
- Update `README.md` if you change the build steps or add a dependency.

## Reporting bugs

Use the bug report issue template. Include the input file's container/codec
if the bug is remux-specific (e.g. "MKV, H.265 + Opus") — that's almost
always the first thing needed to reproduce.

## Security issues

Do not open a public issue — see `SECURITY.md`.
 