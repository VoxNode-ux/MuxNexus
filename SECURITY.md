# Security Policy

## Reporting a vulnerability

Please do **not** open a public GitHub issue for security vulnerabilities.

Instead, use GitHub's private vulnerability reporting:
`Security` tab → `Report a vulnerability` on this repository, once it's
pushed to GitHub (enable it under Settings → Security → Code security).

Alternatively, email the maintainer directly (add your contact email here
before publishing).

## Scope

This app is offline by design: no `INTERNET` permission, and the native
FFmpeg build is compiled with `--disable-network`. The realistic attack
surface is therefore local: malformed/malicious input media files handed to
the demuxer, and Storage Access Framework URI permission handling. Reports
in either of those areas are especially welcome.

## Supported versions

Only the latest release on `main` is supported with security fixes while
this project is pre-1.0.
