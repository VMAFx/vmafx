<!-- markdownlint-disable MD013 MD060 -->
# Research-3403: Shared FFmpeg fix series forge API download fallback

- **Status**: Completed
- **Workstream**: [ADR-3403](../adr/3403-ffmpeg-series-forge-api-fallback.md), [ADR-3143](../adr/3143-ffmpeg-shared-fix-series.md)
- **Last updated**: 2026-11-09

## Question

How can `scripts/ci/ffmpeg-shared-series.sh` reliably retrieve shared FFmpeg fix series release assets when public anonymous HTTP download endpoints encounter CDN Gateway Timeouts (HTTP 504), while preserving artifact verification, eliminating deadline process races, and strictly bounding wall-clock execution?

## Sources

- `docs/adr/3403-ffmpeg-series-forge-api-fallback.md`: Decision record for the forge API fallback mechanism.
- `curl(1)` man page options: `--max-time <seconds>` (`-m`), `--retry <num>`, `--fail` (`-f`), `--silent` (`-s`), `--show-error` (`-S`), `--location` (`-L`), `--output <file>`. Per the man page, `--max-time` limits the maximum time for each transfer operation; `--retry` resets the timer on each retry attempt.
- `wget(1)` man page options: `--timeout=<seconds>` (`-T`), `--tries=<number>`, `--output-document=<file>`, `--quiet` (`-q`). Per the man page, `--timeout` sets DNS, connect, and read timeouts, bounding idle network waits rather than total transfer time.
- Coreutils `timeout(1)` and `gtimeout(1)` man pages: Run a command with a time limit, sending SIGTERM on deadline expiration.
- Perl `perlfunc(1)` documentation: Process management functions `fork`, `alarm`, `waitpid`, `exec`, and `$SIG{ALRM}` signal handlers.
- `scripts/ci/ffmpeg-shared-series.sh`: Implementation of the release download and verification pipeline.
- Controller ledger `vmafx-frontier-feed-20261108-1000-0630-noted` (2026-11-09, paraphrased).

## Findings

1. **Anonymous CDN 504 outage vs. authenticated forge API**:
   On 2026-11-09, direct anonymous HTTP requests to `https://github.com/VMAFx/ffmpeg-patches/releases/download/...` encountered repeated HTTP 504 Gateway Timeouts from upstream edge proxies. However, the authenticated GitHub Releases forge API (`gh release download`) remained operational. Incorporating `gh release download` as an authenticated in-band fallback resolves this failure class without modifying upstream trust roots or lowering verification thresholds.

2. **Exact-host scoping and stream size cap**:
   Fallback execution is restricted strictly to release asset URLs matching:
   `^https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)/releases/download/([A-Za-z0-9_.+-]+)/([A-Za-z0-9_.+-]+)$`
   Other hosts, plain HTTP URLs, and lookalike domains do not trigger the fallback and fail immediately. The downloaded stream is piped to `head -c $((DOWNLOAD_MAX_BYTES + 1))` with `DOWNLOAD_MAX_BYTES = 67108864` (64 MiB); exceeding this limit deletes the destination file and aborts execution.

3. **Deadline semantics and bounded transfer**:
   - `curl --max-time S` with `--retry 3` resets the per-attempt timer upon retry, allowing a stalling transfer to take up to $(1 + 3) \times S$ seconds.
   - `wget --timeout=S` bounds only idle network wait periods (DNS, connect, read), allowing trickle-stalled connections to exceed the expected deadline indefinitely.
   - Wrapping both `curl` and `wget` direct invocations in `run_with_deadline "$DOWNLOAD_SECONDS"` establishes an absolute wall-clock limit of $S$ seconds on the direct path.

4. **Call site inventory and worst-case calculation**:
   Within a single invocation of `fetch_series`, at most $N = 3$ download operations occur:
   - The series tarball (`ffmpeg-patches-<tag>.tar.gz`)
   - The checksum list (`SHA256SUMS`)
   - The cryptographic signature bundle (`SHA256SUMS.sigstore.json`)
   Each operation performs a direct download bounded by $S$ seconds, followed by a fallback attempt bounded by $S$ seconds if the direct attempt fails. The worst-case total download duration is $N \times 2 \times S = 3 \times 2 \times S = 6 \times S$ (720 seconds at default $S = 120$). The wrapper in `scripts/ci/ffmpeg_patch_stack.py` uses this bound to set `timeout = max(600, N * 2 * S + 120)`; unset uses $S = 120$ (840 s).

5. **Parent-supervised deadline helper**:
   An asynchronous watcher process killing a target PID after background `sleep` suffers from PID reuse races: if the command exits, bash reaps it on SIGCHLD, permitting the OS to assign the recycled PID to an unrelated process before the watcher is stopped.
   Parent-controlled execution terminates the child before reaping:
   - Primary: coreutils `timeout` when on PATH.
   - Secondary: `gtimeout` when on PATH.
   - Tertiary: `perl` with `fork()`, `alarm($seconds)`, a `$SIG{ALRM}` handler sending SIGTERM to the unreaped child PID, and `waitpid()`.
   - Fail closed: If none of the three tools exists on PATH, the helper prints a note naming the missing tools and exits non-zero.

6. **Workflow token configuration**:
   `gh release download` in GitHub Actions requires `GH_TOKEN` or `GITHUB_TOKEN`. Steps invoking `ffmpeg-shared-series.sh`, `ffmpeg_patch_stack.py`, or `build-and-run.sh` must specify `GH_TOKEN: ${{ github.token }}` in their step `env:` configuration.

## Alternatives explored

- **Wait for forge recovery**: Rejected. Blocks development and CI whenever public release asset CDNs experience transient outages.
- **Hand-seeded local cache**: Rejected. Violates clean container build isolation and prevents hermetic CI verification.
- **Third-party mirror**: Rejected. Introduces extra infrastructure, mirror latency, and external trust root risks.
- **Forge API via `gh release download`**: Chosen. Directly accesses authoritative release assets over the authenticated API while enforcing sha256, checksum, and signature checks.

## Open questions

None.

## Related

- [ADR-3403](../adr/3403-ffmpeg-series-forge-api-fallback.md) — Shared FFmpeg fix series forge API download fallback
- [ADR-3143](../adr/3143-ffmpeg-shared-fix-series.md) — VMAFx consumes the shared FFmpeg fix series
- [FFmpeg patch automation](../development/ffmpeg-patch-automation.md#shared-ffmpeg-fix-series)
