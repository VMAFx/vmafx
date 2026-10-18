<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1487: code inherited from Netflix/vmaf evaluates as Netflix's source does, a difference needs an ADR, and a guard compares every emitted value against the recorded upstream head

- **Status**: Accepted
- **Date**: 2026-10-02
- **Deciders**: lusoris
- **Tags**: `upstream-divergence`, `upstream`, `parity`, `testing`, `numerics`, `ci`, `rc3`, `fork-local`

## Context

The fork inherited its CPU extractors from Netflix/vmaf and has changed them
since: fixes, ports, lint and CodeQL sweeps. Nothing compared the result with
Netflix's. The Netflix golden gate asserts scores at four decimal places.
`testdata/bench_upstream_ab.py` ([ADR-1228](1228-upstream-ab-perf-milestone.md))
compared the pooled `vmaf` of the two command-line tools at six decimals with
a ceiling of `1e-5`, against upstream `v3.2.0`.

An audit on 2026-10-02 compared the two trees through the C API at `%.17g`:
16 shared extractors, 31 fixtures, 185 option variants, 25 model files, scalar
and default dispatch. The fork differed from Netflix `cea2b4d8` on 170,344 of
348,132 values. Six causes were unintended: `(double)` casts and `float`
forms that a CodeQL cleanup (fork #552, May 2026) and three ports (fork #44,
fork #213, fork #760) had put into Netflix's expressions. The integer ADM cast moved
`vmaf_v0.6.1` by up to 1.8e-5 for five months, under every gate. Fifteen more
differences were
deliberate, and eight of those had no ADR of their own
([ADR-1479](1479-ciede-422-chroma-subsampling-flags.md) to
[ADR-1486](1486-float-motion-scale1-uses-callers-stride.md) record them).

Two findings after the audit shaped the guard. Netflix's own values depend on
the environment: its `ciede2000()` calls `powf(x, 2)`, which on a host with
glibc 2.44 differs from the product on 119 of 327 frames (fork PR #1892) and
in the dev container image (glibc 2.43) on none. And some of upstream's
outputs are read from memory it never wrote: repeating the full matrix on
the host with the heap unfilled and filled with two different bytes moved 48
upstream runs (`ciede` on odd sizes, `float_motion`'s scale-1 chroma,
`float_adm` and `float_vif` on frames below 16 pixels, the SpEED prescale
overrun) and twice took a difference above the bound the guard had given it,
while none of this tree's 5,349 runs moved.

## Decision

**The rule.** Every expression the fork inherited from Netflix/vmaf evaluates
as the recorded Netflix head evaluates it. A difference is allowed only when
an ADR records why the fork differs, and it is listed with its measured size.
An unintended difference is reverted to upstream's expression on the CPU
scalar path, and every SIMD path and every GPU twin declared exact follows.

**The guard.** `scripts/dev/upstream_parity.py` (`make upstream-parity`,
`make upstream-parity-full`) enforces the rule:

1. *Two builds.* Netflix/vmaf at the recorded parity head: the heading
   "Upstream head the fork is at parity with" in
   `docs/development/known-upstream-bugs.md`, read through
   `scripts/ci/upstream_parity_pin.py`, which the licence provenance job
   already reads ([ADR-1474](1474-relicense-helper-headers-and-ci-check.md)).
   This tree with the golden build profile
   (`scripts/ci/setup-golden-build.sh`,
   [ADR-1317](1317-golden-gate-build-isolation.md)).
   *One environment*: both builds and every run happen in the dev container
   image (`--container`, what the make targets pass), and both result
   documents record its id, the compilers and the C library. Two documents
   from different environments are not compared. Outside the image the
   guard measures only with `--unpinned`, and its verdict is then marked
   advisory: it is not evidence and does not move a bound.
2. *One harness.* `scripts/dev/upstream_parity_harness.c`, compiled against
   each tree's static library, runs a request through the C API and prints
   every value of the feature collector at `%.17g`: per-frame values,
   aggregates, mean and harmonic-mean pools.
3. *A matrix.* `scripts/dev/upstream_parity_matrix.py`: the 16 extractors
   both trees have, their option variants, the model files upstream ships,
   on the Netflix pairs and on clips derived from them (crops down to 8x8,
   odd sizes, 4:2:2, 4:4:4, 4:0:0, 10 and 16 bits, full-range noise), on the
   scalar path and the default dispatch (the full matrix adds AVX2). A probe
   set runs in minutes; the full matrix is the whole audit.
4. *An allowlist.* One fragment per difference under
   `scripts/ci/upstream_parity.d/` (the `exact_twins.d` pattern,
   [ADR-1428](1428-exact-twins-fragments.md)): kind, scope, bound, ADR,
   upstream pull request. A difference is attributed to one fragment, a
   deliberate one before a pending one, then the smallest bound.
5. *The failures.* A difference no fragment covers; a difference above its
   fragment's bound; a fragment nothing is attributed to any more (stale); a
   crash of this tree's harness. A comparison that could not be made (no pin,
   no fixtures, a failed build, the wrong environment) exits 2 and is never a
   pass.
6. *The heap check* (`--heap-check`, what `make upstream-parity-full` passes)
   runs every request of both trees again with `MALLOC_PERTURB_=170`. An
   output of this tree that changes fails the guard. An upstream output that
   changes is undefined: a fragment may cover it only with `bound: inf`, and
   a finite bound over it fails the guard.

Two fragment kinds are temporary. `pending-revert` is an unintended
difference whose revert is not on master yet, `pending-port` an upstream
change that is not ported yet; both name the branch that ends them. When the
branch lands, the fragment matches nothing, the guard fails as stale, and the
fragment is removed. No ADR stands behind a pending fragment: it records a
defect, not a decision.

**What this ADR itself records** as deliberate, because no other ADR does:

- *`float_ms_ssim`'s separable decimation* stays
  ([ADR-0125](0125-ms-ssim-decimate-simd.md)). ADR-0125 gives no size against
  upstream; the size is the guard's bound: 1.8e-6 on the score (8e-8 on
  natural content), 7.4e-5 on the dB scale, 9.3e-6 on a per-scale term.
- *Frames this tree refuses where upstream reads outside its buffers and
  returns a value*: `float_vif` below 16 pixels (fork #855, reported upstream
  as Netflix/vmaf#1582), `speed_temporal` on frames too small for one block
  (fork #1029). They are fragments of kind `error`
  ([ADR-1481](1481-extractor-failure-fails-the-run.md) makes the refusal
  reach the caller). The ADM extractors' refusal below 17x17 has its own
  record, [ADR-1494](1494-adm-refuses-frames-below-17.md).
- *`psnr_hvs` on 4:0:0 input* scores luma here; upstream refuses the format
  at init. A fragment of kind `name`.
- *The A/B bench* (`testdata/bench_upstream_ab.py`) builds upstream through
  the guard and takes its score verdict from the guard's comparison of the
  model on the bench's fixtures. This replaces ADR-1228's pooled six-decimal
  delta and its `--max-score-delta` ceiling, and the bench's default upstream
  becomes the recorded parity head instead of the tag `v3.2.0`.

**Decisions taken with it** (maintainer, 2026-10-02): the SpEED float forms
are reverted on the CPU and on every twin (a `pending-revert` fragment until
then); `motion_five_frame_window` is ported, CPU and twins (a `pending-port`
fragment until then); five deviations are offered upstream as pull requests
(Netflix/vmaf#1665, #1666, #1667, #1668; for `ciede` 4:2:2 another
contributor's #1611 exists).

The guard is not a required check. It runs locally (`make upstream-parity`
before a change to an inherited extractor is pushed; `make
upstream-parity-full` when the pin moves and nightly on the maintainer's
workstation). A hosted nightly job is proposed, not wired:
`T-UPSTREAM-PARITY-GUARD-HOSTED-JOB-2026-10-02`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the bench's ceiling (`1e-5` on the pooled score at six decimals) | Exists; one number | It is what the integer ADM cast sat under for five months: one model, one metric, six decimals, three fixtures | It measures the wrong thing at the wrong precision |
| Rely on the Netflix golden gate | Required check; Netflix's own assertions | Four decimal places, default options, three clips; all six unintended differences pass it | The gate proves the fork is close, not that it is the same |
| A tolerance per metric instead of an allowlist | Simple to state | A tolerance hides the next unintended difference of the same size, and says nothing about why the fork differs | The rule is identity; a deviation is a decision with an ADR, not a tolerance |
| Compare the two command-line tools | No harness | Upstream prints six decimals; aggregates and debug metrics are not reachable | Not bit-level |
| Make the guard a required check now | Cannot regress | Needs a second build of another repository and the dev image on every pull request, and its bounds are measured in one image on one host so far | Wire it after a hosted run in the same image has confirmed the bounds; until then it must not report a pass it did not earn |
| Compare on whatever host runs the guard (its first version) | No container needed | Upstream's value moves with the C library (`ciede` above): a bound measured on one host fails or hides a difference on another | A comparison is evidence only in one recorded environment |
| Give undefined upstream values a finite bound, as for any other difference | One rule for every fragment | The bound records one run's heap contents: the next run exceeds it (`float_motion` 25.2, then 61) | A bound must be one that a rerun cannot break |
| Track upstream `master` instead of a recorded head | Always current | A Netflix push would turn the guard red on an unrelated change | The recorded head moves with a reviewed port, as for the licence provenance job |

## Consequences

- **Measured on master** `96af5b34e` with this guard (2026-10-03, dev
  image `sha256:43ef1e32cb32`, GCC 15.2.0, glibc 2.43, a reference workstation,
  against Netflix `9e48141b`), full matrix with the heap check: 5,349 runs
  per tree, 886,002 values, 780,741 identical, 83,516 differences, each
  covered by one of 42 fragments (37 deliberate, 5 pending); none uncovered,
  none above its bound, no fragment stale; 1,675 upstream outputs in 45 runs
  change with the heap fill, none of this tree's. Probe set: 1,848 runs,
  252,162 values, 26,138 covered differences, pass. The table is in
  [upstream parity](../development/upstream-parity.md#result-on-master).
- **The pending fragments end as designed**: four of the five reverts landed
  while this guard was in review (integer ADM, `ciede`, `float_adm`,
  `psnr_hvs`: fork PRs #1891, #1892, #1894, #1895) and their seven fragments
  went stale and were removed. With the last one, the SpEED revert, applied
  in a scratch copy, the full matrix in the same image has 795,000 of
  886,002 values identical, nothing outside the allowlist, and exactly its 3
  `pending-revert` fragments stale.
- **[ADR-1467](1467-ciede-squares-as-products.md)'s product has no fragment**:
  in the dev image upstream's `powf(x, 2)` returns the same values, so there
  is no difference to cover; on a host with glibc 2.44 there is (above).
- **Positive**: an edit that changes one bit of an inherited extractor's
  output, at any option, fails `make upstream-parity` unless an ADR and a
  fragment say why. A port that moves the pin is checked against the head it
  claims.
- **Positive**: the pending reverts cannot be forgotten: each has a fragment
  that fails the guard when its branch has landed, until it is removed.
- **Negative**: the guard needs the dev image, Netflix/vmaf fetched and
  built, and the golden-profile build of this tree in that image: 129 s for
  the probe set from an empty work directory, both builds included, and
  about ten minutes for the full matrix with the heap check (565 s of runs
  for both trees twice; eight workers, a busy host). Results are cached per harness binary, environment
  and heap fill.
- **Negative**: the bounds are measured in one image (GCC 15.2.0, glibc
  2.43) on x86-64, on the scalar, AVX2 and AVX-512 paths. A run anywhere
  else is advisory; a new image is a new environment, whose first full run
  re-measures every bound.
- **Negative**: where upstream's value is undefined the bound is `inf`, so
  the guard cannot see a change of this tree's value there
  (`ciede.odd-size-chroma`, `float_motion.scale1-stride`,
  `speed_temporal.prescale-above-one`); the tests of ADR-1483, ADR-1486 and
  ADR-1480 hold this tree's values.
- **Not compared**: aarch64 and NEON, clang, icx and MSVC builds; more than
  one thread; `n_subsample`; pooling methods other than mean and harmonic
  mean; the extractors only this tree has and `float_ansnr`, which only
  upstream has; the GPU twins (they are held to the CPU by the exact-twin
  gate, [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md)).
- **Neutral / follow-ups**: a new deliberate deviation is one ADR and one
  fragment in the same pull request; a pull request that moves the pin runs
  the full matrix and removes the fragments its port makes stale.

## References

- [Upstream parity guide](../development/upstream-parity.md);
  [allowlist](../development/upstream-parity-allowlist.md).
- ADR-1228 (the bench this ADR changes), ADR-1317 (golden build profile),
  ADR-1428 (fragment pattern), ADR-1474 (the recorded upstream head),
  [ADR-1494](1494-adm-refuses-frames-below-17.md) (the ADM refusal below
  17x17); fork PR #1892 (upstream's `powf(x, 2)` on glibc 2.44).
- Source: `req` (popup answer, 2026-10-02): "Netflix's source, deviations
  only by ADR".
- Source: `Q` (popup answers, 2026-10-02): SpEED float forms: "Revert CPU and
  all twins (Recommended)"; `motion_five_frame_window`: "Port now, CPU and
  twins (Recommended)"; `float_ms_ssim` separable decimation: "Keep, bounded
  by the guard (Recommended)"; the five deviations D3, D4, D5, D9, D10: "All
  five as PRs (Recommended)".
