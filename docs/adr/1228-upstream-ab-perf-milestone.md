<!-- markdownlint-disable MD013 MD060 -->
# ADR-1228: A recurring "faster than upstream, and still exact" milestone

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: performance, benchmarking, cuda, sycl, hip, process, fork-local

## Context

The fork carries a large amount of performance machinery: four GPU backends,
AVX2 / AVX-512 / NEON paths, per-architecture CUDA codegen, and a benchmark
tree (`testdata/bench_backends.py`, `bench_all.sh`, `docs/benchmarks.md`,
ADR-1185's baseline methodology) that is genuinely disciplined about warmup,
medians, spread and load average.

All of it compares the fork against **itself** — one backend against another,
or one commit against a recorded baseline. Nothing in the repository answers
the two questions that decide whether any of it was worth doing:

1. **Is the fork faster than the thing it forked?**
2. **Did it stay numerically exact while getting there?**

The first measurement taken to answer them is not flattering, which is the
point of taking it. On a 48-frame 1920x1080 pair, single-threaded, CPU path,
against upstream `v3.2.0`:

```text
upstream v3.2.0   median 0.756 s
fork              median 0.764 s
speedup           0.989x
pooled vmaf       upstream 39.582252   fork 39.582257   delta +5.0e-06
```

Two findings, both of which only exist because the comparison was made:

- **The fork is at parity with upstream on the single-threaded CPU path**, not
  ahead of it. Its advantage is the GPU backends and the added feature surface,
  not CPU throughput. Any claim otherwise was unmeasured.
- **The fork's VMAF score differs from upstream's by ~5e-6.** All fourteen
  pooled features agree exactly at the 6 decimals the output format exposes;
  only the final score moves, by up to 8e-6 per frame, in both directions. That
  is under the Netflix golden gate's `places=4`, which is why nothing caught
  it.

Hardware is also not a fixed backdrop. NVIDIA, Intel and AMD ship new
architectures and new toolchains continuously — this fork alone moved to CUDA
13.3 with an sm_80 floor (ADR-1223) and ROCm 10 (ADR-1225) inside one week.
Code that was optimal for one generation is not automatically optimal for the
next, and a tuning decision taken once decays silently.

## Decision

We will run an **upstream A/B milestone** on a recurring trigger, gated on
numerical parity, and treat hardware-generation retuning as part of it rather
than as occasional opportunistic work.

**1. The A/B harness.** `testdata/bench_upstream_ab.py` clones and builds
upstream at a pinned tag, runs both binaries over the same fixtures with the
ADR-1185 discipline (one discarded warmup, `--runs` timed repetitions reported
as the median, spread and load average recorded), and emits two columns per
cell: **speedup** and **score delta**.

The CPU path is the only honest A/B surface. Upstream has no SYCL, HIP or Metal
backend and its CUDA backend covers a different feature set, so a GPU
comparison would measure the hardware rather than the work.

**2. Parity gates the speed number.** A speedup bought by changing the score is
a regression with a nice number attached. The harness fails the run when the
pooled delta exceeds `--max-score-delta`, independently of timing. The ceiling
starts at `1e-5` — above the 1e-6 floor the `%.6f` output format imposes, and
above today's observed 5e-6 — so the gate catches the delta **growing** while
the existing one is being localised.

**3. The recurring trigger.** The milestone runs when any of these fire:

| Trigger | Why |
| --- | --- |
| Before every release | the release notes should not claim performance nobody measured |
| A Renovate bump to CUDA, ROCm or oneAPI | a new toolchain re-tunes register allocation, vectorisation and scheduling underneath us |
| A new upstream release tag | the baseline moved; the speedup column is meaningless against a stale pin |
| Any PR whose stated purpose is performance | otherwise the claim rests on a microbenchmark of the thing that was changed |

**4. Hardware-generation retuning is part of the milestone, not a side quest.**
The fork already varies code by architecture — the CUDA gencode set, the
`hip_gfx_targets` fat-binary list, the SYCL sub-group sizes, the x86 ISA
dispatch. Each of those is a tuning decision with a shelf life. On every
toolchain bump the milestone re-checks, per backend:

- the target architecture list is still the right one (new arch shipped? floor
  still justified?);
- occupancy and register-pressure assumptions still hold, since a new `ptxas`
  or `hipcc` re-allocates registers without asking (the worked example: a kernel
  assumed occupancy-bound turned out grid-bound, and
  `__launch_bounds__` measured as a 3.4% *regression* — ADR-1226);
- the numbers in `docs/benchmarks.md` are re-measured rather than inherited.

**5. Findings are recorded, not just observed.** Every run updates
`docs/benchmarks.md`; every unexplained delta opens a `docs/state.md` row. The
5e-6 score delta above is filed as
`T-UPSTREAM-AB-SCORE-DELTA-2026-09-07`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep benchmarking the fork against itself only | Already built and disciplined | Answers "did this commit regress?" but never "is the fork worth using?" — and it structurally cannot surface a shared-with-upstream inefficiency, because both sides of the comparison carry it | Rejected — it is the gap this ADR exists to close |
| A/B the GPU backends against upstream's CUDA too | Covers where the fork actually invests | Upstream's CUDA backend implements a different feature subset, so the comparison measures coverage, not speed. Cross-backend numbers already live in `bench_backends.py` | Rejected for the A/B specifically; per-backend work continues in its own harness |
| Track upstream `master` instead of a pinned tag | Always current | The baseline would move under every rerun, so the speedup column would measure upstream's churn as much as the fork's work; a recorded table would not be comparable to itself month to month | Rejected — pin, and bump the pin deliberately |
| Fail the run on any non-zero score delta | Strictest possible parity | Fails every run today on a known 5e-6 delta, which trains everyone to pass `--max-score-delta` and stop reading the column | Rejected — ratchet on growth, and file the known delta as a tracked bug |
| Run it in CI on every PR | Never forgotten | Cloning and building upstream plus a meaningful (multi-second) workload is far too slow for per-PR CI, and a shared runner cannot produce a stable median anyway | Rejected — trigger-driven on the bench host, like the ADR-1185 baselines |

## Consequences

- **Positive**: the fork's performance claims become measured rather than
  assumed; a numerical divergence from upstream that the golden gate's
  `places=4` hides is now visible and bounded; toolchain bumps carry an
  explicit retuning step instead of silently invalidating old tuning.
- **Negative**: the milestone needs the bench host and a large fixture, so it
  cannot be a per-PR gate — it depends on being triggered. The upstream build
  adds a clone plus a CPU-only meson build on first run (cached thereafter).
- **Neutral / follow-ups**: the tracked fixtures are all startup-dominated
  (every cell runs in well under a second, so the speedup column sits near
  1.00x by construction). The harness now warns when that is the case, but the
  real fix is fetching the 4K pair; `MIN_USEFUL_SECONDS` encodes the threshold.
  Localising the 5e-6 delta is tracked separately.

## References

- req: the user asked for a recurring milestone covering (1) A/B performance
  against original Netflix upstream, (2) finding more performance without
  dropping precision, and (3) continuous retuning as Intel, AMD and NVIDIA ship
  new hardware generations and framework versions.
- [ADR-1185](1185-backend-perf-baseline-methodology.md) — the measurement
  discipline this inherits.
- ADR-1226 (in flight, PR #1388) — worked example of a tuning assumption that
  measurement overturned: a kernel believed occupancy-bound was grid-bound, and
  the "obvious" `__launch_bounds__` fix measured as a 3.4% regression.
- ADR-1223 (in flight, PR #1384) and ADR-1225 (in flight, PR #1386) — the CUDA
  and ROCm toolchain moves that make retuning recurring rather than one-off.
- Upstream Netflix/vmaf — <https://github.com/Netflix/vmaf>
