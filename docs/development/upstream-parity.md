<!-- markdownlint-disable MD013 MD060 -->
# Upstream parity guard

Code this fork inherited from Netflix/vmaf returns what Netflix's source
returns. A difference is allowed only when an ADR records why the fork
differs, and each one is listed with its measured size
([ADR-1487](../adr/1487-upstream-parity-policy-and-guard.md)). The upstream
parity guard checks this: it builds Netflix/vmaf and this tree in the dev
container image, runs both through the C API and compares every value they
emit, bit for bit.

```bash
make upstream-parity        # probe set: about two minutes from nothing
make upstream-parity-full   # every fixture, option variant and model, twice: about ten
```

Both targets need the dev image (`vmaf-dev-mcp:local`, built by
`docker compose -f dev/docker-compose.yml build dev-mcp`, see
[the dev container guide](dev-mcp.md)); `UPSTREAM_PARITY_IMAGE` names
another one.

`upstream parity: PASS` means every value both trees emit is identical or
covered by a recorded deviation, and every recorded deviation still exists.
Run the probe set before pushing a change to an inherited extractor
(`core/src/feature/` files that Netflix has too, the picture and pooling code
of `core/src/`), and the full matrix when the recorded upstream head moves.

## What is compared

| | Upstream side | This tree |
| --- | --- | --- |
| Source | Netflix/vmaf at the recorded parity head: the heading "Upstream head the fork is at parity with" in [known upstream bugs](known-upstream-bugs.md), read by `scripts/ci/upstream_parity_pin.py` | the checkout the guard runs in |
| Build | `meson setup` of `libvmaf/`, release, `enable_float`, CPU only, with the compilers of this tree's golden build | the golden build profile, `scripts/ci/setup-golden-build.sh` ([ADR-1317](../adr/1317-golden-gate-build-isolation.md)), in the guard's work directory |
| Environment | the dev container image, for both builds and every run (below) | the same image |
| Program | `scripts/dev/upstream_parity_harness.c`, linked statically against that tree's `libvmaf.a` | the same source |

### The harness

The harness makes one request through the C API (`vmaf_use_feature()` or a
model load, `vmaf_read_pictures()`, the flush) with one worker thread, and
prints everything the feature collector holds at `%.17g`: each per-frame
value, each aggregate, and the mean and harmonic-mean pool of each metric.
Neither tree's `vmaf` tool can serve: upstream's prints six decimals.

### Request set

The requests come from `scripts/dev/upstream_parity_matrix.py`:

- **Extractors**: the 16 both trees register: `adm`, `vif`, `motion`,
  `float_adm`, `float_vif`, `float_motion`, `float_ssim`, `float_ms_ssim`,
  `float_psnr`, `float_moment`, `psnr`, `psnr_hvs`, `ciede`, `cambi`,
  `speed_chroma`, `speed_temporal`; at their default options and at 169
  option variants.
- **Models**: the 24 model files upstream ships under `model/` and four
  built-in versions, with `transform` and `noclip` for two of them.
- **Dispatch**: `scalar` (`cpumask` 63) and `default` (`cpumask` 0); the full
  matrix adds `avx2` (`cpumask` 48) on x86.

The probe set is every extractor at its defaults on 18 of the fixtures, the
option variants on `nflx8`, `cb1` and `noise8`, and seven models on the same
three. The full matrix is every fixture, the option variants on five
fixtures and every model on eight.

### Fixtures

| Fixtures | Source | Needed |
| --- | --- | --- |
| `nflx8`, `cb1`, `cb10` | the three Netflix golden pairs, `scripts/test/fetch-test-yuvs.sh` | yes |
| `noise8` to `noise16`, `nflx422`, `nflx444`, `nflx400`, `nflx10w`, `nflx16w`, `odd444`, `s256x144` down to `s8x8` | derived by the guard from the 576x324 pair: full-range noise (SHAKE-256), chroma repeated to 4:2:2 and 4:4:4, luma only, samples widened to 10 and 16 bits, top-left crops | no file needed |
| `nflx10`, `nflx12`, `nflx16`, `nflx422p10`, `sparks10`, `q160x90`, `akiyo352`, `akiyo18x22` | the other Netflix clips under `python/test/resource/yuv` | compared when present |
| `bbb4k` | `testdata/bbb` (6 frames) | compared when present |

A fixture whose file is absent is listed as `fixture not compared` and its
runs are not counted.

### What counts as a difference

- **Value**: the two printed numbers differ. Equal text is equal; two NaNs
  are equal whatever their sign; `0` against `-0` is a difference of size 0;
  a NaN or an infinity on one side is a difference of infinite size.
- **Status**: the run ends differently. `ok` (the request completed, with or
  without values), `error` (an API call failed, or the tree printed an error
  of its own) or `crash` (a signal). Both sides failing the same way is not a
  difference. A crash of this tree's harness always fails the guard.
- **Name**: a metric only one tree emits in a run both complete.
- **Pooled values** follow from the per-frame values. Where those differ, the
  pooled difference is counted as derived and not bounded. Where the frames
  are identical, a pooled difference is a difference in the pooling code and
  is treated like any value.

### The pinned environment

Netflix's own values depend on the compiler and the C library they are built
and run with. Its `ciede2000()` squares with `powf(x, 2)`: in the dev image
(GCC 15.2.0, glibc 2.43) that returns this tree's product on every frame the
guard compares, while on a host with glibc 2.44 it differs on 119 of 327
frames by up to 2.2e-11 (fork PR #1892). A comparison is therefore evidence
only in one recorded environment, and the guard has one: the dev container
image.

- `--container [IMAGE]` (what both make targets pass) starts the guard in
  the image with no network, as the calling user, with the checkout mounted
  at its own path, and fetches the recorded head on the host first.
- Both result documents record the environment: the image id, the compilers'
  versions, the C library and the machine. The report's first line names it.
  Two documents from different environments are not compared (exit 2).
- Outside the image the guard refuses to measure (exit 2). `--unpinned`
  measures anyway and marks the verdict `(advisory: environment not pinned)`;
  such a result is not evidence of parity and does not move a bound.
- Builds and the run cache are kept per environment under
  `build-upstream-parity/` (`image-<id>/`, `host/`), so a host build is
  never reused in the image.
- On x86-64 the upstream build is configured with `-march=x86-64`, the
  pinned image's compiler default. Netflix's build leaves floating-point
  contraction at the compiler's default, and a host compiler that defaults to
  x86-64-v3 (Ubuntu's `amd64v3` variant, as on the hosted CI runners) would
  fuse its multiply-adds and move its scores; the fork's build turns
  contraction off on every file. A host build configured before this pin is
  reused as it is: remove `build-upstream-parity/host/` to reconfigure it.

### Undefined upstream values: the heap check

Some of upstream's outputs are read from memory it never wrote: the guard
cannot bound a difference from a value that changes with what the heap
held. `--heap-check` (what `make upstream-parity-full` passes) runs every
request of both trees a second time with `MALLOC_PERTURB_=170`, which makes
glibc fill fresh allocations, and compares each tree with itself:

- An output of **this tree** that changes fails the guard: it reads memory it
  did not write (the defect class of
  `T-FLOAT-SSIM-SUB-WINDOW-SIMD-COUNT-2026-10-02`).
- An **upstream** output that changes is undefined. A fragment may cover it,
  but only with `bound: inf`; a finite bound over it fails the guard, because
  a later run with other heap contents would exceed it.

In the dev image 1,675 upstream outputs in 45 runs change with the heap fill:
`ciede` on the odd-sized crops, `float_motion` with `motion_add_scale1` and
`motion_add_uv`, and `float_adm` and `float_vif` on the 12x9 and 8x8 crops.
None of this tree's outputs does.

### What is not compared

aarch64 and NEON; clang, icx and MSVC builds; any environment but the dev
image; more than one thread;
`n_subsample`; pooling methods other than mean and harmonic mean;
`float_ansnr` (upstream only); the extractors and options only this tree has;
the GPU twins, which the [cross-backend gate](cross-backend-gate.md) holds to
the CPU.

## Running it

```bash
scripts/dev/upstream_parity.py --container                     # = make upstream-parity
scripts/dev/upstream_parity.py --container --mode full --heap-check   # = make upstream-parity-full
scripts/dev/upstream_parity.py --container --only F.ciede      # runs whose name contains the text
scripts/dev/upstream_parity.py --container --dispatch scalar
scripts/dev/upstream_parity.py --container --summary           # group what is not covered
scripts/dev/upstream_parity.py --container --write-json out/   # keep both result documents
scripts/dev/upstream_parity.py --from-json out/upstream.json out/fork.json
scripts/dev/upstream_parity.py --unpinned                      # on the host: advisory only
```

| Exit status | Meaning |
| --- | --- |
| 0 | parity holds |
| 1 | a difference is not covered or is above its bound, a fragment is stale, this tree's harness crashed, or (heap check) an output of this tree depends on the heap or a finite bound covers an undefined upstream value |
| 2 | the comparison could not be made: no recorded head, a required fixture missing, a failed fetch or build, a malformed fragment, outside the pinned environment without `--unpinned`, documents from two environments |

The guard fetches `master` of `https://github.com/Netflix/vmaf.git` into
`FETCH_HEAD` (`--no-fetch` reuses the one present), exports the recorded
commit, and builds under `build-upstream-parity/` (ignored by git). Run
results are cached there per harness binary, environment and heap fill, so a
second run of an unchanged tree only compares. `UPSTREAM_PARITY_JOBS`
(default 8) sets the workers of both builds and of the runs.

With `--only` or `--dispatch` the run set is a slice of the matrix. A slice
cannot tell a stale fragment from one whose witness was not run, so stale
fragments are reported there and do not fail.

## The allowlist

One file per difference under `scripts/ci/upstream_parity.d/`, named
`<extractor>.<topic>` (`model.<topic>` for predicted scores). The generated
table is [allowed differences from Netflix/vmaf](upstream-parity-allowlist.md).

<!-- >>> CHART upstream-parity-allowlist: generated, do not edit -->
<!-- markdownlint-capture -->
<!-- markdownlint-disable MD013 MD033 -->
<figure class="vx-chart" markdown>

![37 allowlist entries over 12 extractors (25 value, 10 error, 2 name); the most are for adm 9, float_adm 5, float_ms_ssim 4.](../assets/charts/upstream-parity-allowlist.light.svg#only-light){ .vx-chart__static width="602" height="386" }
![37 allowlist entries over 12 extractors (25 value, 10 error, 2 name); the most are for adm 9, float_adm 5, float_ms_ssim 4.](../assets/charts/upstream-parity-allowlist.dark.svg#only-dark){ .vx-chart__static width="602" height="386" }

<figcaption markdown>Source: the fragments in `scripts/ci/upstream_parity.d/`, one per recorded difference.</figcaption>

</figure>

<details class="vx-chart__table" markdown>
<summary>Data table</summary>

| Extractor | `value` | `error` | `name` | Total |
| --- | ---: | ---: | ---: | ---: |
| `adm` | 7 | 2 | 0 | 9 |
| `float_adm` | 4 | 1 | 0 | 5 |
| `float_ms_ssim` | 4 | 0 | 0 | 4 |
| `ciede` | 2 | 1 | 0 | 3 |
| `psnr` | 2 | 0 | 1 | 3 |
| `psnr_hvs` | 1 | 1 | 1 | 3 |
| `speed_temporal` | 2 | 1 | 0 | 3 |
| `model` | 2 | 0 | 0 | 2 |
| `speed_chroma` | 0 | 2 | 0 | 2 |
| `cambi` | 0 | 1 | 0 | 1 |
| `float_motion` | 1 | 0 | 0 | 1 |
| `float_vif` | 0 | 1 | 0 | 1 |

</details>
<!-- markdownlint-restore -->
<!-- <<< CHART upstream-parity-allowlist -->

### Fragment keys

| Key | Meaning |
| --- | --- |
| `kind` | `value`, `error`, `name`, `pending-revert` or `pending-port` |
| `runs` | globs over the run name: `F.<extractor>.<options>`, `M.<model>`, `C.<collection>`, `B.<built-in>`. Default `F.<extractor>.*` from the file name |
| `metrics` | globs over the metric name (`value`, `name`) |
| `fixtures` | globs over the fixture name; default every fixture |
| `frames` | globs over where the value sits: a frame number, `agg`, `pool-mean`, `pool-harmonic`; default all |
| `dispatch` | `scalar`, `avx2`, `default`; default all |
| `bound` | the largest absolute difference covered; `inf` covers a NaN or an infinity on one side, and is the only bound allowed where upstream's value is undefined (the heap check) |
| `status` | `<upstream>/<fork>` pairs of `ok`, `error`, `crash` (`error`; also allowed on a `value` whose upstream side is undefined behaviour that sometimes ends the run instead) |
| `side` | `fork` or `upstream`: the tree that emits the name (`name`) |
| `adr` | the ADR(s) that record the deviation; required unless the fragment is pending |
| `upstream` | `Netflix/vmaf#<n>`: the pull request or issue that would end the deviation. Optional: leave it out while none exists and add the line when one is opened |
| `branch` | the branch that ends a pending difference |
| `evidence` | one line: what was measured |

A difference is attributed to exactly one fragment that covers it: a
deliberate fragment before a pending one, then the one with the smallest
bound. A fragment to which no difference is attributed is **stale** and fails
the guard. That is what ends a pending fragment: once its revert or port is on
master, nothing is attributed to it any more.

### Adding a deviation

1. Write the ADR: upstream's behaviour (file and line at the recorded head),
   this tree's, why it differs, and what would end it. No ADR, no fragment.
2. Measure in the image: `scripts/dev/upstream_parity.py --container --mode
   full --heap-check --summary` lists what is not covered, grouped by run,
   metric and fixture, with the largest difference of each group.
3. Add the fragment. Scope it as narrowly as the deviation: the option that
   triggers it, the fixtures it shows on, the dispatch if it is a SIMD
   matter. The bound is the measured maximum, rounded up to two digits, or
   `inf` where the heap check finds upstream's value undefined. A bound is
   not a tolerance: it records how far the deviation reaches, and a larger
   difference later is a finding. The `evidence` line names the environment
   it was measured in.
4. `make docs-fragments-write` regenerates the table, and
   `make upstream-parity-full` must pass with no stale fragment.

A fragment that should disappear (an unintended difference whose revert is in
progress, an upstream change that is being ported) is `pending-revert` or
`pending-port` with its `branch`. It needs no ADR. The pull request that
lands the branch removes the fragment; if it does not, the guard fails as
stale on master until someone does.

## When the recorded upstream head moves

A port or sync changes the heading in
[known upstream bugs](known-upstream-bugs.md) (id and date only; the licence
provenance job reads the same heading). In the same pull request:

1. `make upstream-parity-full` against the new head.
2. A difference that appears is either what the port brought in wrongly (fix
   the port) or something upstream changed that the port left out (a
   `pending-port` fragment naming the branch that will port it, or the port
   itself).
3. A fragment that goes stale because upstream took the fork's fix is
   removed, and its ADR gets a superseding note in the port's ADR.

## Where it runs

Locally, in the dev image. The guard is not a required check and no hosted
job runs it yet (`T-UPSTREAM-PARITY-GUARD-HOSTED-JOB-2026-10-02` in
[`state.md`](../state.md)): a hosted job would have to build or pull the same
image, and its bounds are measured on one host so far. The maintainer's
workstation runs the full matrix nightly:

```bash
make upstream-parity-full UPSTREAM_PARITY_JOBS=8 > upstream-parity.log 2>&1 || echo "upstream parity: exit $?"
```

!!! warning
    A run that did not happen is not a pass. Read the log's last line
    (`upstream parity: PASS` or `FAIL`) or the exit status, never their
    absence.

The script's own tests need no build:
`scripts/ci/tests/test_upstream_parity_allowlist.py`
and `scripts/dev/tests/test_upstream_parity.py` (a pre-commit hook runs both).

## The A/B bench

`testdata/bench_upstream_ab.py`
([ADR-1228](../adr/1228-upstream-ab-perf-milestone.md))
times upstream against this tree. It builds upstream through the guard, at the
recorded head unless `--upstream-ref` names another commit or tag, and takes
its score verdict from the guard: the model `vmaf_v0.6.1` on the bench's
fixtures, every value at `%.17g`, against the allowlist. The pooled delta it
prints beside the timing is informational. The bench times on the host, so
its verdict there is marked advisory; run in the dev image it is the guard's.

## Result on master

!!! note
    This section is a dated measurement, not a standing claim. It goes stale
    at the next upstream head move; re-measure with
    `make upstream-parity-full`.

Full matrix: measured on 2026-10-03 in the rebuilt dev image
(`sha256:14db4c43070a`, GCC 15.2.0, glibc 2.43) on `ryzen-4090-arc` (Ryzen 9
9950X3D): master `f65dc6969` against Netflix `9e48141b`, after every revert
and port landed. The allowlist holds 37 fragments, all deliberate deviations;
none is pending. Every bound measured in the previous image
(`sha256:43ef1e32cb32`) held unchanged in this one. Probe set: master
`623b11ce2` in the previous image, before the SpEED revert and the
five-frame motion port landed.

| | Probe set | Full matrix |
| --- | --- | --- |
| Runs per tree | 1,848 | 5,349 |
| Runs both trees complete / both fail / end differently | 1,734 / 18 / 96 | 4,962 / 93 / 294 |
| Values compared | 254,922 | 911,802 |
| Identical | 221,936 | 820,800 |
| Pooled values that follow a per-frame difference | 6,590 | 20,512 |
| Differences covered by a deliberate fragment | 19,958 | 70,820 |
| Differences covered by a pending fragment | 6,550 | 0 |
| Not covered / above the bound / stale fragments | 0 / 0 / 0 | 0 / 0 / 0 |
| Heap check: upstream outputs that change / this tree's | not run | 1,675 in 45 runs / 0 |
| Verdict | PASS | PASS |

### Changes since the probe-set run

What changed on master since the probe-set run:

- The integer ADM, `ciede`, `float_adm` and `psnr_hvs` reverts are on master
  (fork PRs #1891, #1892, #1894, #1895), and so is the five-frame motion port
  (#1887): their fragments are gone.
- The port is exact: the four `vmaf_v1.0.16_hfr` models and
  `motion_five_frame_window` now run on both trees (114 more runs complete),
  and no difference they add is outside the allowlist.
- With the last revert, SpEED, applied in a scratch copy of the tree before
  the port landed, the full matrix in the same image had 795,000 identical
  values of 886,002, no difference outside the allowlist and none above a
  bound, and exactly the 3 `pending-revert` fragments of that branch reported
  as stale. That is the state master reaches when it lands and its fragments
  are removed.

### What no fragment covers

No fragment covers a value of `cambi`, `vif`, `float_vif`, `motion`,
`float_psnr`, `float_moment`, `float_ssim` or `psnr_y`: wherever both trees
return one, it is the same bit pattern, and so are prediction and the pooling
of finite values. Since the reverts, the same holds for `ciede` outside 4:2:2
input and odd sizes, and for every per-frame value of `psnr_hvs`.

Three bounds are `inf` because upstream's value is undefined there:
`ciede.odd-size-chroma` (upstream reads past its rounded-down chroma planes),
`float_motion.scale1-stride` and `speed_temporal.prescale-above-one`. On the
host, before the heap check existed, filling the heap moved the first two
above the finite bounds they had then (0.3 and 26).

### Run time

Time on that host, eight workers, with other builds running: from an empty
work directory the probe set took 129 s in all, the export, both builds and
the fixtures included (108 s of runs); the full matrix with the heap check
took 565 s of runs for both trees twice. A second run of an unchanged tree
takes seconds.

## Unmodified upstream consumer ABI parity gate

The standard parity harness links statically and instruments internal hooks (`--wrap=vmaf_feature_collector_init`). To prove that the fork's shared library `libvmaf.so.3` is an exact drop-in ABI replacement for Netflix's `libvmaf.so.3`, the ABI parity gate compiles an unmodified upstream consumer against Netflix public headers and swaps the dynamic library at runtime via `LD_LIBRARY_PATH` ([ADR-3339](../adr/3339-compat-abi-parity-upstream-consumer.md)).

```bash
make compat-abi-parity        # probe matrix against unmodified consumer
make compat-abi-parity-plant  # runs with LD_PRELOAD plant; asserts failure (exit 1)
```

### What it proves

1. **Drop-in binary substitution**: `scripts/dev/compat_abi_consumer.c` uses strictly public Netflix C API headers (`libvmaf/libvmaf.h`, `model.h`, `picture.h`). It is compiled once against upstream headers and linked dynamically with `-lvmaf` to upstream `libvmaf.so.3`.
2. **Dynamic loader verification**: The driver asserts via `ldd` and `readelf -d` that upstream resolves `libvmaf.so.3` (SONAME 3), and fork resolves `libvmaf.so.3` which in turn dynamically links `libvmafx.so.1` (SONAME 1). No silent fallbacks to system libraries are permitted.
3. **Bit-identical public output**: The consumer discovers metric names through `vmaf_write_output()` JSON export into an ephemeral file, queries frame and pooled scores via public getters (`vmaf_feature_score_at_index`, `vmaf_feature_score_pooled`), and formats them identically to the static harness.

### Planted defect verification

To verify that the gate refuses regressions, `scripts/dev/compat_abi_plant.c` provides an `LD_PRELOAD` shim intercepting `vmaf_feature_score_pooled()` and `vmaf_score_pooled()`, moving pooled results by 1 ULP via `nextafter()`. Running with `--plant` injects this shim on the fork side: the gate detects the uncovered perturbation and exits with status 1 (`EXIT_DIFFERENT`), reporting the difference. `make compat-abi-parity-plant` runs this check and asserts failure.
