<!-- markdownlint-disable MD013 -->
# Research-1188 — percentile temporal pooling: what the two surfaces actually compute

Companion digest for [ADR-1188](../adr/1188-percentile-pooling-methods.md) and the
ledger row `T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03`
(upstream `Netflix/vmaf#818`). Everything below was measured on this tree at
`origin/master` (`cd52f2670`) plus the branch's change, CPU backend, 2026-09-06.

## 1. What the gap actually was

`Netflix/vmaf#818` reads as two claims. Only one survives inspection:

| Claim | Verdict | Evidence |
| --- | --- | --- |
| The C pooling enum has no percentile methods | **True** | `enum VmafPoolingMethod` at `core/include/libvmaf/libvmaf.h` listed `{UNKNOWN, MIN, MAX, MEAN, HARMONIC_MEAN, NB}`; no `PERC` / `MEDIAN` enumerator anywhere under `core/include/`. |
| An unknown pooling method silently falls back to mean | **Refuted** | `pool_reduce()` ends in `default: return -EINVAL;`, and all three public entry points reject `VMAF_POOL_METHOD_UNKNOWN` up front. A pre-fix probe asking for discriminants 5–8 returned `rc=-22` for every one. |

So the defect was expressive, not numerical: no input reached wrong behaviour,
but a C / Rust / FFmpeg consumer could not name the percentile the Python
harness has always offered.

## 2. Which interpolation rule the harness uses

`compat/python-vmaf/core/result.py` routes the aggregate through
`ListStats.perc10`, which is `numpy.percentile(scores, 10)`. NumPy's default
`method="linear"` computes the rank as `q·(n−1)/100` and interpolates linearly
between the two neighbouring order statistics. That is the same expression the
fork already had in `core/src/predict.c` for the bootstrap `ci_p95` bounds:

```c
const double p = perc * (n_scores - 1) / 100.;
return scores[idx_l] * (idx_r - p) + scores[idx_r] * (p - idx_l);
```

Matching rules is the whole point: a "nearest rank" or "exclusive" definition
would have made the C API and the harness disagree on the same frames — which
is the class of defect the report complains about. The unit test pins the
difference: on `[1, 2, 3, 4]` the linear rule gives `perc5 = 1.15`,
`perc10 = 1.3`, `perc20 = 1.6`, `median = 2.5`; a nearest-rank implementation
would return `1.0 / 1.0 / 2.0 / 2.0`.

## 3. Cross-checking against the golden pair

The 48 per-frame VMAF scores of the Netflix golden pair
(`src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv`, `vmaf_v0.6.1`,
captured with `--json --precision=max`) give:

| Rank | `numpy.percentile` of the CLI's frames | C `vmaf_feature_score_pooled` |
| --- | --- | --- |
| perc5 | 72.351853083385384 | identical to < 1e-12 |
| perc10 | 72.717340155042217 | identical to < 1e-12 |
| perc20 | 73.357468139344396 | identical to < 1e-12 |
| median | 76.091664004240072 | identical to < 1e-12 |

`python/test/quality_runner_test.py:679` pins the harness's own perc10 for the
same clip pair at **72.71845922683059**, i.e. 1.1e-3 away from the C engine's
number. That residual is *not* a pooling difference — it is the per-frame score
difference between the harness's run and the CLI's, and it sits inside the
`places=2` (5e-3) tolerance the golden assertion itself uses. The regression
test asserts both: exact agreement with NumPy on the shared vector, and
agreement with the golden constant at the golden tolerance.

## 4. Why the accumulator path was left alone

`MEAN` and `HARMONIC_MEAN` are golden-pinned, and ADR-1118 requires their
unweighted expressions to stay byte-identical to upstream. Deriving them from a
sorted vector would reorder the summation, so the implementation keeps the
running accumulators untouched and adds an *optional* second consumer of the
same frame walk. Cost profile:

| Methods | Memory | Time | Golden risk |
| --- | --- | --- | --- |
| MIN / MAX / MEAN / HARMONIC_MEAN | O(1) | O(n) | none — expressions unchanged |
| MEDIAN / PERC5 / PERC10 / PERC20 | O(n), `8·n` bytes | O(n log n) | none — new code path |

The open-ended `vmaf_score_pooled(..., 0, UINT_MAX)` idiom documented in
`docs/api/index.md` is why the buffer grows geometrically instead of being sized
from `index_high − index_low`: the latter would ask for 32 GiB.

## 5. Weighted percentiles: considered and rejected

ADR-1118 perceptual weighting turns `MEAN` / `HARMONIC_MEAN` into their weighted
forms and deliberately leaves `MIN` / `MAX` alone, because re-weighting cannot
reorder per-frame scores. The same reasoning applies to percentiles, and there
is a stronger argument: `ListStats` has no weighted-percentile notion, so a
weighted C rank would reintroduce exactly the C-vs-Python divergence this change
exists to remove. The header documents the behaviour rather than hiding it.

## 6. Verification commands

```bash
meson setup build-cpu core -Denable_cuda=false -Denable_sycl=false
ninja -C build-cpu
./build-cpu/test/test_pool_percentile            # 6/6 pass
meson test -C build-cpu --suite=fast             # 115/115 OK
CUDA_VISIBLE_DEVICES="" VMAF_FORCE_BACKEND=cpu PYTHONPATH=$PWD/python \
  python3 -m pytest python/test/quality_runner_test.py python/test/feature_extractor_test.py \
    python/test/vmafexec_test.py python/test/vmafexec_feature_extractor_test.py \
    python/test/result_test.py -q -m "not slow"  # 271 passed, 12 skipped
```
