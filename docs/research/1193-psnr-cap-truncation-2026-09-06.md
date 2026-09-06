<!-- markdownlint-disable MD013 MD060 -->
# Research 1193 — the PSNR `psnr_max` ceiling truncates real scores

Companion to [ADR-1193](../adr/1193-psnr-uncapped-option.md). Closes
`T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03` (Netflix/vmaf#1109).

## The question

Netflix/vmaf#1109 reports "PSNR value is capped". The
[ADR-1166 harvest](1166-upstream-issue-harvest-2026-09-03.md) verified the
report against this tree and left a `docs/state.md` row rather than a fix,
because closing it needs a new user-visible option surface. This digest
records what was measured before the fix landed, and why the fix takes the
shape it does.

## What the ceiling is for

Both PSNR extractors need a finite number when the reference and distorted
planes are byte-identical: `sse == 0`, so `10 * log10(peak^2 / 0)` is `+inf`,
and neither the JSON/XML/CSV writers nor the pooling code can carry an
infinity. The chosen stand-in is `psnr_max`:

| Extractor | `psnr_max` |
|---|---|
| `psnr` (integer) | `(6 * bpc) + 12` → 60 / 72 / 84 / 108 dB at 8 / 10 / 12 / 16 bpc |
| `float_psnr` | a fixed 60 / 72 / 84 / 108 dB table keyed on bpc |

Both were implemented as a single `MIN(...)` over the *computed* value with
the MSE floored at `1e-16` (`1e-10` for the float path). That produces the
sentinel as a side effect — a floored zero MSE yields ≥ 208 dB, which the
`MIN` collapses to `psnr_max` — and truncates everything else at the same
time. The two behaviours were never separable.

That "collapses to `psnr_max`" step is *nearly* always true, and the near is
why the fix is shaped the way it is. `min_sse` raises `psnr_max` to
`ceil(10 * log10(peak^2 * n / min_sse))`; below `min_sse ≈ 1.9e-11` on a
576x324 frame that ceiling passes 208 dB, and the shipped code then reports
the floored 208 dB rather than the ceiling for an identical pair. A fix that
re-derives the default as "`mse == 0` → `psnr_max`" would silently change that
corner. So the `uncapped == false` arm in every one of the ten extractors is
the pre-fix expression **verbatim**, not a re-derivation of it, and the
`mse == 0` sentinel branch exists only on the `uncapped` side. The default is
then bit-identical by construction rather than by argument.

## Measurement

Fixture: the first frame of the Netflix golden reference
`src01_hrc00_576x324.yuv` (YUV420p, 8 bpc, 576x324), and a copy with luma
sample 0 raised by one step. That is `sse == 1` over `576 * 324 = 186624`
luma samples, both chroma planes byte-identical.

Ground truth: `10 * log10(255^2 * 186624) = 100.84047854497734` dB.
FFmpeg n9.0.1's own `psnr` filter reports `100.840479` on the same pair.

Pre-fix, `build-cpu/tools/vmaf --feature psnr --feature float_psnr`:

```json
"psnr_y": 60.000000, "psnr_cb": 60.000000,
"psnr_cr": 60.000000, "float_psnr": 60.000000
```

`psnr_cb` / `psnr_cr` at 60 dB are *correct* — those planes are identical,
so 60 dB is the sentinel doing its job. `psnr_y` at 60 dB is a 40.84 dB
truncation of a real measurement.

The existing escape hatch, `--feature psnr=min_sse=0.000001`, gives:

```json
"psnr_y": 100.840479, "psnr_cb": 155.000000, "psnr_cr": 155.000000
```

i.e. it fixes the luma value by *raising the sentinel*, which turns the two
correct chroma readings into two wrong ones. It is also integer-only, is not
mirrored on any GPU twin, and requires the caller to choose an epsilon that
depends on the frame area (`psnr_max = ceil(10*log10(peak^2 * n / min_sse))`).
That is why it is not a sufficient answer to #1109, and why the ledger's
closure condition asked for a separate option.

## Blast radius of the naive fix

Simply deleting the `MIN` would move every score above the ceiling on every
backend. The constraint that rules it out is the Netflix golden gate:

| Assertion family | Pair | `sse` | Pinned value |
|---|---|---|---|
| `quality_runner_test.py`, `vmafexec_test.py`, … | byte-identical references | 0 | 60.0 / 84.0 / 108.0 |

Every golden PSNR assertion is an `sse == 0` pair — they pin the *sentinel*,
not the truncation. So a fix that keeps the sentinel unconditional and makes
only the truncation optional leaves the golden gate bit-identical, which is
what the fork's hard rule requires. Confirmed empirically: the full
`meson test --suite=fast` set (115 tests) is green with the default path, and
`core/test/test_psnr_uncapped.c` asserts the default 60.0 explicitly next to
the uncapped 100.840479.

## Why the option is not a feature param

`vmaf_feature_name_from_options()` suffixes the emitted feature key for any
option carrying `VMAF_OPT_FLAG_FEATURE_PARAM`. The CPU `psnr` extractor
appends with `vmaf_feature_collector_append()` (no name dict); all eight GPU
twins append with `..._append_with_dict()` and a dict built from their own
option table. Flagging `uncapped` would therefore rename the GPU features
(`psnr_y_uncapped`) while leaving the CPU ones alone — a new cross-backend
divergence in exchange for nothing, since the option changes the *value* of
`psnr_y`, not its identity. It is left unflagged.

## Scope correction carried forward

The upstream reporter's concrete symptom — 28 dB where 72 dB was expected —
is **not** this defect and must not be re-attributed to it. A `MIN` can only
lower a value; a reading *below* expectation is a frame-alignment problem in
the caller's decode graph. `docs/state.md` already records that as a separate
"Confirmed not-affected — wrong diagnosis" row, and it stays there.

## Cross-backend status

| Backend | Change | Verified |
|---|---|---|
| CPU (`psnr`, `float_psnr`, `psnr.c`) | option + split branch | yes — `test_psnr_uncapped`, CLI before/after, 115/115 fast tests |
| CUDA (`integer_psnr_cuda.c`, `float_psnr_cuda.c`) | option + split branch | compiled and run on the RTX 4090 |
| SYCL (`integer_psnr_sycl.cpp`, `float_psnr_sycl.cpp`) | option + split branch | compiled and run on the Intel Arc A380 |
| HIP (`integer_psnr_hip.c`, `float_psnr_hip.c`) | option + split branch | compiled |
| Metal (`integer_psnr_metal.mm`, `float_psnr_metal.mm`) | option + split branch | **not executed** — no Apple GPU on this workstation; reasoned from the code only |

GPU scores are not bit-identical to the CPU reference and this change does not
make them so; the parity tolerances in `core/test/test_*_psnr_parity.c` are
unchanged because the default path is unchanged.

## References

- Netflix/vmaf#1109.
- `docs/state.md` — `T-UPSTREAM-1109-PSNR-CAP-TRUNCATES-2026-09-03`.
- [ADR-1166](../adr/1166-upstream-issue-harvest.md) / [Research 1166](1166-upstream-issue-harvest-2026-09-03.md) — the harvest that verified the report.
- [ADR-1193](../adr/1193-psnr-uncapped-option.md) — the decision.
- `docs/metrics/psnr.md` — the user-facing documentation.
