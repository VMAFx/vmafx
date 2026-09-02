<!-- markdownlint-disable MD013 MD060 -->
# ADR-0908: Slow-test audit (2026-05-30) — no >30 s tests found; install `slow` marker as a future gate

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: ci, testing, devx

## Context

The user asked for an audit of slow tests (the ad-hoc gate was "anything
over 30 seconds") across the three pytest packages (`ai/tests/`,
`mcp-server/vmaf-mcp/tests/`, `tools/vmaf-tune/tests/`) and the meson
test suite. The motivation: slow tests inflate every CI round-trip and
every local pre-push gate. A pre-emptive audit catches drift before the
suite is too painful to run on every push.

The empirical result is unambiguous — **no test in any suite exceeds
30 seconds today**. The slowest test in the entire tree clocks 13.40 s
(`tools/vmaf-tune/tests/test_bbb_e2e_v5_bug_cluster.py::test_ladder_against_bbb_container_yields_plausible_vmaf`
— a docker-exec live ladder run against the BBB corpus). The second
slowest is 13.20 s (`test_v14_a_nvenc_probe_succeeds_on_gpu_host`,
a live NVENC encoder probe; skipped on hosts without an NVIDIA driver).
The libvmaf meson suite tops out at 5.00 s (`test_framesync`). The
`ai/tests/` suite tops out at 3.21 s
(`test_train_konvid_mos_head.py::test_smoke_run_is_deterministic`).

Because the threshold question came up at all, the absence of a
registered `slow` pytest marker is itself a gap: when a future test
*does* breach 30 s, there is no documented opt-out mechanism for the
fast-path CI gates.

## Decision

We will:

1. **Register a `slow` pytest marker** in `tools/vmaf-tune/pyproject.toml`'s
   `[tool.pytest.ini_options]` so future >30 s tests have a documented
   opt-out (`pytest -m "not slow"`). Apply the same marker convention
   in the other two pytest packages (`ai/pyproject.toml`,
   `mcp-server/vmaf-mcp/pyproject.toml`) for consistency.

2. **Mark the two ~13 s tests with `@pytest.mark.slow`** even though
   neither breaches 30 s. They are the only tests in the tree that
   plausibly grow past the threshold (one is a real docker-exec ladder
   encode; the other is a live NVENC probe gated by hardware). Marking
   them now means a `-m "not slow"` developer gate excludes them
   without further triage.

3. **Apply a low-risk speedup** to the docker ladder e2e test:
   - Drop the encode `--duration` from 4 s -> 2 s (~50 % less encode +
     decode work; the test's assertion floor is `vmaf >= 50.0`, which
     2 s of BBB content still clears comfortably).
   - Reduce the CRF sweep from 3 points (`23,28,33`) -> 2 points
     (`23,33`). Combined with 2 resolutions this still yields 4
     samples, which is the assertion floor (`len(samples) >= 4`).
   The expected runtime drops from ~13 s -> ~7 s.

We will **not** modify the NVENC probe test — the ~12 s cost is GPU
driver context-creation latency, not test logic, so the only way to
speed it up is to mock the encoder probe (which would defeat the
"live GPU smoke" purpose of the test).

We will **not** touch the meson tests. The slowest one
(`test_framesync` at 5.00 s) is a synchronization-correctness probe
that legitimately needs to drive multiple frames through the pipeline.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Take no action (no tests >30 s today) | Zero diff | Leaves no marker for the next time a test breaches the threshold | Pre-emptive markers cost nothing and prevent the next audit from finding the same gap |
| Add `slow` marker + skip in default CI | Faster CI by ~13 s | Hides real coverage; the two marked tests catch real regressions (V5-2 garbage encode, V14-A NVENC init) | Mark-and-run is safer; the marker exists as opt-out, not opt-in |
| Aggressively speed the v5 docker test (1 res * 1 CRF) | Fastest | Loses the dedup assertion (`len(samples) == len(set(keys))`) and the multi-resolution coverage of V5-3 | 2 res * 2 CRF is the minimum that preserves both invariants |
| Mock the NVENC probe with a fake `runner` | Cuts 13 s -> <1 s | Test no longer probes a real driver; loses the V14-A regression-catching value | The whole point of `test_v14_a_nvenc_probe_succeeds_on_gpu_host` is to catch real GPU init failures |

## Consequences

- **Positive**:
  - Future tests >30 s have a documented and pre-installed opt-out marker.
  - The docker e2e test runs ~45 % faster (~13 s -> ~7 s) without losing coverage.
  - The audit produces a baseline timing table that the next audit can compare against.
- **Negative**:
  - One more marker name developers must remember (`slow`).
- **Neutral / follow-ups**:
  - A research digest accompanies this ADR under
    `docs/research/slow-test-audit-2026-05-30.md` capturing the full
    per-test timing table.
  - If a future test breaches 30 s, the author should mark it `slow`
    and justify in the same PR; reviewers verify against this ADR.

## References

- Source: `req` — "Audit slow tests (>30 sec) across pytest + meson test suites. Document + propose speedups."
- Related: [ADR-0108](0108-deep-dive-deliverables-rule.md) (six deep-dive deliverables).
- Companion digest: [`docs/research/slow-test-audit-2026-05-30.md`](../research/slow-test-audit-2026-05-30.md).
