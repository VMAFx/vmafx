---
paths:
  - testdata/bench_all.sh
  - testdata/perf_multi_resolution.json
invariant: Backend selection flags verify engagement per run; benchmark harness maintains thread and stderr hygiene.
---
<!-- markdownlint-disable MD013 MD060 -->
# Backend engagement verification, bench flags, and perf baselines

## Backend-engagement foot-guns (read before benching)

Two CLI flags govern backend selection at runtime; relationship is
**not** "set flag for backend you want". run that looks like
it's exercising CUDA can silently fall through to CPU and still produce
expected score (because CUDA extractors emit same logical
features). Symptoms reviewers see: bit-exact CPU/CUDA/SYCL pools,
identical fps across backends — **always wrong on non-trivial fixture
size unless flags are right.**

- **`--gpumask` is CUDA *disable* bitmask, not device pin.**
  `compute_fex_flags` ([`src/libvmaf.c::compute_fex_flags`](../src/libvmaf.c))
  enables CUDA dispatch slot only when `gpumask == 0`. Any
  nonzero value disables CUDA. Public-header semantics:
  `if gpumask: disable CUDA` (see
  [`include/libvmaf/libvmaf.h`](../include/libvmaf/libvmaf.h) `VmafConfiguration::gpumask`).
- **`--backend cuda` does engage CUDA — this bullet used to say it did
  not; that is no longer true.** Re-verified on 2026-09-06 at commit
  `cd52f2670` on a reference workstation while refreshing baselines
  (ADR-1185): `--backend cuda` on 200-frame 4K BBB pair runs at
  167.16 fps against CPU's 14.37 fps, emits 14 `frames[0].metrics`
  keys against CPU's 15. Identical scores *and* identical fps would
  be fallback signature; neither holds. Use `--backend $name` as
  canonical exclusive selector. (Historical note, kept because it explains
  older bench rows: CLI once set `gpumask = 1` as device pin while
  runtime read any nonzero `gpumask` as "disable CUDA", so
  `--backend cuda` did in fact run CUDA init, then score on CPU.
  Bench numbers captured while that was live are not CUDA numbers.)
- **Check engagement per run, never trust flag.** Cheap check is
  `frames[0].metrics` key count, which `testdata/bench_backends.py`
  records for every cell. See
  [`docs/development/backend-perf-baselines.md`](../../docs/development/backend-perf-baselines.md).
- **`--no_cuda` / `--no_sycl` are *disable*-only.** Pairing
  `--no_sycl` alone (without `--gpumask`) does NOT enable CUDA — it
  only disables SYCL while leaving CUDA unrequested. CLI inits
  CUDA only when `c.use_gpumask && !c.no_cuda` (see
  [`tools/vmaf.cpp`](../tools/vmaf.cpp) device-init block).

**Correct invocations for backend bench / cross-backend diff:**

| intent | flags |
|---|---|
| CPU only | `--no_cuda --no_sycl` |
| CUDA | `--gpumask=0 --no_sycl` |
| SYCL | `--sycl_device=0 --no_cuda` |

Verify engagement by recording each run's JSON `frames[0].metrics`
key count. Never encode permanent expected counts: exposed feature set
changes as extractors evolve. GPU count collapsing to CPU count is
fallback warning signal; corroborate it with pool, throughput, and stderr.

Bench script `testdata/bench_all.sh` historically used wrong
flag pattern (`--no_sycl` for "CUDA"). Numbers from runs older than
2026-04-28 in `docs/benchmarks.md` were CPU-on-CPU comparisons. See
[ADR-0064 in rebase-notes](../../docs/rebase-notes.md) and PR #169 for
corrected methodology.

**GPU bench row reading "unavailable" is not evidence backend is
absent** (measured 2026-09-06 on `cd52f2670`). Two invariants bench run must
hold onto:

- **Keep `--threads 1` in GPU bench rows.** PR #1343 / ADR-1197 closed
  `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06`; CUDA and SYCL now match their
  serial paths with worker pool. flag is still load-bearing because it
  exercises `read_pictures_frame_cleanup_after_batch`. For SYCL, that helper
  is reached only after `threaded_read_pictures_batch` has waited on
  `last_upload_event` while retaining caller's original picture refs;
  waiting in helper itself is too late if worker already dropped its
  copies (BUG-040). Never drop flag to make bench row green.
- **Never discard binary's stderr in bench harness.** `bench_all.sh` used
  to send it to `/dev/null`, relabel any non-zero exit as "backend likely
  unavailable"; that turned hard abort into row that looked like missing
  device for months. Capture stderr, print exit code, let reader
  decide what it means.

**Dated observation, not invariant:** on 2026-09-06 at `cd52f2670`,
FFmpeg filter path emitted 15 keys for CPU, 14 for CUDA, and 24 for SYCL
(35 before PR #1324). Use counts as "did backends run different code"
signal, never as fixed constants.

## Performance benchmark invariant (ADR-0752)

- **`testdata/perf_multi_resolution.json` is versioned performance baseline.**
  Any PR claiming performance improvement (CPU/CUDA/SYCL throughput, latency)
  must re-run `scripts/perf/bench-multi-resolution.sh` with same
  `--backends` and `--resolutions` flags. Include structured diff table
  in PR description (see `docs/development/perf.md §Comparing PR against
  baseline`).
- If PR intentionally changes throughput (optimisation or trade-off),
  commit updated `testdata/perf_multi_resolution.json` with justification
  in commit message.
- Upscaled fixture files (`testdata/ref_1920x1080_48f.yuv`, `testdata/ref_2560x1440_48f.yuv`,
  `testdata/dis_1920x1080_48f.yuv`, `testdata/dis_2560x1440_48f.yuv`) are
  generated on first run, are **not committed** (reproducible via
  `ffmpeg -vf scale=W:H:flags=bilinear` from in-tree 576×324 fixture).
