# ADR-0110: Coverage gate `-fprofile-update=atomic` for parallel meson tests

- **Status**: Superseded by [ADR-0111](0111-coverage-gate-gcovr-with-ort.md)
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, build, simd, testing
- **Note**: The two race fixes documented here (`-fprofile-update=atomic`
  and `meson test --num-processes 1`) remain in force; ADR-0111 layers
  the lcov→gcovr migration and the ORT install on top.

## Context

The Coverage gate (CPU job in `.github/workflows/ci.yml`) fails reliably
on `master` after the AVX-512 SIMD rollouts. The hard-fail line is
emitted by `lcov`/`geninfo` during the "Gather lcov report" step:

```text
geninfo: ERROR: Unexpected negative count '-44224' for
  /home/runner/work/vmaf/vmaf/core/src/feature/x86/vif_avx2.c:673.
        Perhaps you need to compile with '-fprofile-update=atomic'
```

Mechanism: meson runs the unit-test suite in parallel
(`meson test -j $(nproc)`); every test binary links the same
instrumented `libvmaf.so`; SIMD inner loops increment the same
`.gcda` 64-bit counters concurrently. The default
`-fprofile-update=single` performs a non-atomic read-modify-write,
which loses updates and can produce *negative* counter values.
`geninfo` refuses to process a negative count and exits non-zero,
so the coverage artifact is never produced and the gate fails.

The downstream symptom (`test_run_vmafexec FAILED` + `frame_skipping
Timeout` in the pytest step) is unrelated CI noise: that step runs
under `|| true` and never fails the gate by itself; the orphan
`vmaf` subprocesses get killed in the runner cleanup.

## Decision

The fix has two parts that address two different races:

1. **Intra-process race** (multi-threaded SIMD inner loops within one
   test binary): compile the coverage build with
   `-fprofile-update=atomic` for both C and C++ TUs, in both the CPU
   and the (advisory) GPU coverage jobs. Atomic counter RMW closes
   the race that produced the negative-count `geninfo` abort.
2. **Inter-process race** (multiple parallel test binaries merging
   their counters into the same `.gcda` files for the shared
   `libvmaf.so`): pass `--num-processes 1` to `meson test` in the
   coverage steps so test binaries run serially. Without this, the
   on-exit `.gcda` merge from concurrent processes still corrupts
   counters even with the atomic build flag — observed as
   `dnn_api.c — 1176%` line coverage in the first
   `-fprofile-update=atomic`-only attempt (and asymmetrically low
   counts on neighbouring DNN files), because `lcov` does not
   serialise the file-level merge that gcov runtime performs at
   process exit. `-fprofile-update=atomic` is per-thread, not
   per-process; the two fixes are complementary, not alternatives.

As a belt-and-suspenders measure, also pass `--ignore-errors negative`
to `lcov --capture` so a future SIMD addition or coverage-step change
that reintroduces a small race window degrades to a warning instead
of a hard gate failure.

The flags are applied only in the coverage CI workflow, not the
default build, so production builds are unaffected.

Serialisation cost: meson test runs ~2× slower in the coverage step
(unit suite goes from ~30s wall-time to ~60s on `ubuntu-latest`),
which is a rounding error against the 30-min job budget and the
20-min Python pytest section.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `-fprofile-update=atomic` + `meson test --num-processes 1` + `--ignore-errors negative` (chosen) | Fixes both intra- and inter-process races correctly; gate produces honest, consistent per-file coverage; robust to future SIMD growth | Coverage build is ~5% slower (atomic RMW); test step is serial (~2× wall-time on the unit suite) | Correct fix; cost is acceptable for a once-per-CI-run job |
| `-fprofile-update=atomic` alone (initial attempt) | One-flag fix; geninfo no longer aborts | Multi-process .gcda merge still races → inflated hit counts on some files (e.g. `dnn_api.c — 1176%`) and undercounts on others; per-file gate becomes meaningless | Verified empirically on `fix/lint-exclude-upstream-mirror-tests` build of 2026-04-18 — geninfo was happy but per-file numbers were nonsense |
| `--ignore-errors negative` alone (no atomic, no serialisation) | One-line workaround | Counter values silently wrong; coverage % becomes meaningless on instrumented SIMD lines | Defeats the purpose of having a coverage gate |
| Skip the failing test cases | Trivial diff | The tests aren't the cause; they are downstream noise. Skipping hides a real product bug if one ever appears here | User explicitly rejected this approach during scope discussion ("who said skip?") |
| Switch to `gcovr` with per-process `.gcda` directories via `GCOV_PREFIX` | Eliminates inter-process race without serialising tests | Requires per-test wrapper script + lcov→gcovr migration; new tooling surface to maintain | Larger blast radius than the race justifies; serialisation is a one-flag change |

## Consequences

- **Positive**: Coverage gate becomes deterministically green on
  `master` again; merge queue unblocks. The fix is local to the CI
  workflow — no source-tree changes, no surprises in production
  builds.
- **Positive**: Future AVX-512 / NEON SIMD additions don't reopen
  this race because atomic counters are now in force for the
  coverage build.
- **Negative**: ~5% slower coverage build (atomic RMW vs. plain RMW
  on each instrumented basic-block edge) plus ~2× wall-time on the
  meson test step (serial vs. parallel). Both negligible against the
  20-minute pytest timeout budget already in place.
- **Negative**: `--ignore-errors negative` masks future races *if*
  they reintroduce themselves and either `-fprofile-update=atomic`
  or `--num-processes 1` is somehow disabled. Mitigation: the `lcov`
  step still prints warnings, which surface in the job log.
- **Neutral / now-visible**: with race-free counters, the gate now
  exposes a real, pre-existing coverage gap on DNN critical files
  (model_loader, onnx_scan, op_allowlist, tensor_io, opt,
  read_json_model — all 5–18% line coverage). Closing that gap is
  out of scope for this ADR; tracked as a follow-up workstream
  ("ramp coverage on DNN critical files toward the 85% bar").
- **Neutral / follow-ups**: When the GPU coverage gate is promoted
  from advisory to required (post-runner-stability window), confirm
  the same flags are still in force on `coverage-gpu`. ADR-0037's
  required-status-check list will need `coverage-gpu` added at that
  point.

## References

- Master CI run that exposed the issue: <https://github.com/VMAFx/vmafx/actions/runs/24605954678>
  (and four prior consecutive failures on `master` since the AVX-512
  motion port landed).
- GCC docs on `-fprofile-update`: <https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html>
- `lcov` `--ignore-errors` reference:
  <https://github.com/linux-test-project/lcov/blob/master/man/lcov.1>
- Related ADRs: [ADR-0015](0015-ci-matrix-asan-ubsan-tsan.md) (sanitizer
  CI matrix — coverage runs in the same family),
  [ADR-0037](0037-master-branch-protection.md) (required status checks
  list).
- Source: `req` — direct user direction on this PR
  ("we should up the coverage soon I guess" + rejection of skip-style
  fixes: "who said skip?").
