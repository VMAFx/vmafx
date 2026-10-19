- CI: `core/test/test_pelorus_udu_wrap.c` uses `NULL` again, so the
  `Windows ARM64 MSVC` and `Windows MSVC+CUDA` builds compile it (C2065 on
  `nullptr`, ADR-1138). The `msvcism` stage of `scripts/dev/preflight.sh` no
  longer drops its findings when the last file it scans is clean, finds its
  static-aggregate scanner outside this repository too, and runs in the
  required `Pre-Commit` job on every pull request
  ([ADR-3351](docs/adr/3351-msvcism-scan-on-light-tier.md),
  [preflight](docs/development/preflight.md)).
