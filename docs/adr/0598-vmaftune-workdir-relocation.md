<!-- markdownlint-disable MD013 MD060 -->
# ADR-0598: vmaf-tune workdir relocation — disk-space preflight + VMAFTUNE_WORKDIR env var

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Sonnet 4.6)
- **Tags**: `vmaf-tune`, `bugfix`, `cli`, `container`, `workspace`

## Context

`vmaf-tune compare` (and `ladder`, `tune-per-shot`) decode the reference
source to raw YUV before the bisect loop so each encode can be scored
against frame-aligned raw content. For a 634-second 1080p60 BBB source
decoded to `yuv420p`, the raw file is approximately 118 GB
(`1920 × 1080 × 1.5 bpp × 38 040 frames`). The `dev-mcp` container's
`/tmp` is an 8 GB `tmpfs`, so `ffmpeg` silently exits with return code
228 (the unsigned 8-bit representation of `−28 = −ENOSPC`). All 44 rows
in the BBB compare run failed with this opaque exit code; no human-readable
diagnostic was emitted and the error was indistinguishable from a codec
initialization failure.

Two independent problems required fixing:

1. **No disk-space guard.** `_encode_and_score` launched `ffmpeg`
   unconditionally; the first signal that disk space was insufficient
   came from `ffmpeg`'s exit code after several seconds of wasted I/O.

2. **No routing to a larger volume.** Even if the caller knew to set a
   different workdir, `bisect.py` had no API knob and all
   `TemporaryDirectory` calls were rooted at the OS default (`/tmp`).

## Decision

Three orthogonal changes land together:

1. **Disk-space preflight.** Before invoking the reference decode,
   `_estimate_yuv_bytes` computes an upper-bound byte count
   (`width × height × bpp × max(frames, 1)`, where `bpp` is looked up
   from a `pix_fmt` table that conservatively defaults to `1.5`). If
   `shutil.disk_usage(workdir).free < estimated_bytes × 1.1`, the bisect
   returns `BisectResult(ok=False, error=<human-readable message>)`
   immediately, without touching `ffmpeg`. The error string names both the
   `--workdir` CLI flag and the `VMAFTUNE_WORKDIR` environment variable so
   the operator has an actionable remedy.

2. **`VMAFTUNE_WORKDIR` environment variable.** `_workdir_parent()` reads
   `os.environ.get("VMAFTUNE_WORKDIR", "")` and returns a `Path` when set.
   `bisect_target_vmaf` passes the result as the `dir=` argument to
   `tempfile.TemporaryDirectory`, routing all scratch I/O to the
   operator-specified volume. Resolution order: explicit `workdir=` kwarg >
   `VMAFTUNE_WORKDIR` env > OS default.

3. **`--workdir PATH` CLI flag.** Added to the `compare`, `tune-per-shot`,
   and `ladder` subparsers. The flag's value is forwarded to
   `bisect_target_vmaf` as the `workdir=` kwarg.

4. **Container default.** `dev/Containerfile` sets
   `ENV VMAFTUNE_WORKDIR=/probes/vmaftune-work`; the `/probes`
   bind-mount (`source: ./.workingdir/dev-mcp-probes`, `target: /probes`)
   provides approximately 435 GB of writable space on the host.
   `dev/scripts/dev-mcp-entrypoint.sh` creates the directory at startup
   with `mkdir -p`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Raise `ENOSPC` to a Python exception | Cleaner caller semantics | Breaks the `BisectResult` contract; all callers check `result.ok` | The structured error return is the existing convention |
| Catch ffmpeg rc=228 and retry on a different path | No API change required | Fragile: rc=228 is not exclusively ENOSPC; also requires a retry path with bookkeeping | Preflight is more reliable and fires before wasting I/O |
| Hard-code `/probes/vmaftune-work` in bisect.py | Zero configuration burden | Not portable outside the dev container | Env var gives operators control without code changes |
| Use `XDG_RUNTIME_DIR` or `TMPDIR` instead of a new env var | Familiar convention | Changing `TMPDIR` redirects all temp I/O (Python, ffmpeg, system tools) — too broad | Dedicated `VMAFTUNE_WORKDIR` is scoped to vmaf-tune only |

## Consequences

- **Positive**:
  - BBB 634 s 1080p60 compare runs no longer silently fail with rc=228.
    The preflight fires immediately with a human-readable error and the
    `--workdir` / `VMAFTUNE_WORKDIR` remedy.
  - Operators on space-constrained hosts can route scratch I/O to any
    mounted volume without modifying code.
  - The container environment is zero-configuration: `VMAFTUNE_WORKDIR`
    is pre-set to the 435 GB `/probes` bind-mount.

- **Negative**:
  - Disk-space estimation is conservative (worst-case upper bound for the
    given `pix_fmt`). A host with 90–100 GB free and a 118 GB estimate
    will be refused even though the actual raw YUV might be slightly
    smaller. The 10 % headroom margin is intentional.

- **Neutral / follow-ups**:
  - `_estimate_yuv_bytes`, `_check_disk_space`, and `_workdir_parent` are
    private helpers (not in `__all__`). Tests import them directly for unit
    coverage; they are not part of the stable public API.
  - The `/probes` bind-mount's 435 GB capacity is shared across all vmaf-tune
    scratch runs; parallel BBB runs can collectively exceed it on a
    long-running dev session. No automatic cleanup is implemented; operators
    should periodically purge `$VMAFTUNE_WORKDIR`.

## References

- ADR-0498 (BBB e2e v2 bug cluster — container-source decode root cause):
  [`0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md`](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)
- Source: `req` — user direction (2026-05-18, paraphrased): the BBB e2e
  v13 compare run failed all 44 rows with rc=228 (ENOSPC). The full 634 s
  1080p60 source decodes to raw YUV of approximately 118 GB, but the
  dev-mcp container's `/tmp` is an 8 GB tmpfs. Fix: add a disk-space
  preflight, honour `VMAFTUNE_WORKDIR` for workdir routing, add
  `--workdir PATH` to `compare`, `tune-per-shot`, and `ladder`, and set
  `VMAFTUNE_WORKDIR=/probes/vmaftune-work` in the container.
