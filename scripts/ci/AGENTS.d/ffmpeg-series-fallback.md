---
paths:
  - scripts/ci/ffmpeg-shared-series.sh
  - scripts/ci/test_ffmpeg_shared_series_fallback.py
  - .github/workflows/ffmpeg-patch-stack.yml
  - .github/workflows/ffmpeg-integration.yml
invariant: Release fallback exact github.com host only; 64 MiB cap; total deadline bound; GH_TOKEN on calling steps.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# FFmpeg shared series release download fallback (ADR-3403)

`scripts/ci/ffmpeg-shared-series.sh` falls back to `gh release download` via forge API on release asset 504 failures.

- Exact-host restriction: URL matches `^https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)/releases/download/([A-Za-z0-9_.+-]+)/([A-Za-z0-9_.+-]+)$`. Other hosts and lookalike domains bypass fallback; fail immediately.
- Size cap: Fallback stream piped to `head -c` bounded at 64 MiB (`DOWNLOAD_MAX_BYTES = 67108864`). Exceeding bound deletes destination and aborts.
- Total deadline bound: Direct download (curl/wget) and fallback (gh) paths bounded by `run_with_deadline "$DOWNLOAD_SECONDS"`. Coreutils `timeout` -> `gtimeout` -> `perl` (fork + alarm + waitpid); missing tools fail closed. Worst-case download duration across N = 3 calls (tarball, `SHA256SUMS`, `SHA256SUMS.sigstore.json`): `N * 2 * S = 6 * S`. Wrapper limit `max(600, N * 2 * S + 120)`; unset uses S = 120 (840 s).
- Calling step token: Every workflow step invoking `ffmpeg-shared-series.sh`, `ffmpeg_patch_stack.py`, or `ffmpeg-patches/test/build-and-run.sh` sets `GH_TOKEN: ${{ github.token }}` in `env:`.
