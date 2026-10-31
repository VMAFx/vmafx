# `--precision` — score output precision

`--precision` sets how many digits the `vmaf` CLI prints for each score in XML,
JSON, CSV and SUB output and on stderr. The default is `%.6f`, which matches
upstream Netflix output exactly. Pass `--precision=max` for round-trip lossless
`%.17g`. The flag is fork-added per
[ADR-0119](../adr/0119-cli-precision-default-revert.md), which supersedes
[ADR-0006](../adr/0006-cli-precision-17g-default.md).

## Grammar

```text
--precision N          # integer 1..17 -> printf "%.<N>g"
--precision max        # alias for "%.17g" (IEEE-754 round-trip lossless)
--precision full       # alias for "%.17g"
--precision legacy     # "%.6f": synonym for the default (pre-fork format)
```

Without `--precision` the format is `%.6f`. Any other value is rejected with
`must be an integer 1..17, or one of: max, full, legacy`.

!!! note "Other entry points"
    - The `vmafx` alias defaults to `--precision=max`; `--netflix-compat`
      forces the `%.6f` default back. See [vmafx-cli.md](vmafx-cli.md).
    - In FFmpeg, the `score_fmt` option of the `vmafx` filter does the same
      job for the report file: `score_fmt=%.17g` is the lossless form
      ([ffmpeg.md](ffmpeg.md#vmafx-filter-option-table)).

## When to pick each

| Mode | Use when |
| --- | --- |
| no flag / `legacy` | **Default, Netflix-compatible.** Matches upstream byte for byte. Required for the golden gate, FFmpeg `vf_libvmaf` and any consumer that parses the existing CLI output schema. |
| `max` / `full` / `N = 17` | Cross-backend numeric diff (CPU vs SIMD vs CUDA vs SYCL), archival reports where every ULP matters, or any pipeline that re-parses scores into doubles and compares them. |
| `N = 3` to `N = 6` | Human reading on a terminal where you want shorter scores. Do not pipe this into a tool that compares scores. |

## Effect on all output channels

The same format applies to every channel:

- the pooled stderr line (`vmaf_v0.6.1: 76.667831`), printed only when stderr
  is a terminal;
- XML per-frame attribute values;
- JSON per-frame and pooled numbers;
- CSV cells;
- SubRip subtitle text.

You cannot pick different precisions for different outputs in one invocation.
This is intentional: `output.xml`, `output.json` and the stderr line always
agree.

## Example: round-trip check

This run pins `vmaf_v0.6.1`, so the numbers match the Netflix golden pair.
Without the pin, the default model prints `82.816060` for the same pair.

```shell
./build/tools/vmaf \
  --reference  src01_hrc00_576x324.yuv \
  --distorted  src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model version=vmaf_v0.6.1 \
  --output scores.json --json
```

Default output (Netflix-compatible), pooled `vmaf` entry:

```json
"vmaf": { "min": 71.174759, "max": 87.180962, "mean": 76.667831, "harmonic_mean": 76.508907 }
```

The same run with `--precision max`:

```json
"vmaf": { "min": 71.174759054828428, "max": 87.180962344894908, "mean": 76.667831491355784, "harmonic_mean": 76.508907413867888 }
```

The default form drops about 9 significant digits. On the three Netflix CPU
golden pairs the default and `max` forms agree to 6 decimals by construction,
but that is exactly the margin at which SIMD and GPU backends deviate. Archival
reports and cross-backend diffs must therefore use `--precision=max`.

## Why `%.6f` is the default

Several Netflix golden tests (for example
[`python/test/command_line_test.py`](../../python/test/command_line_test.py))
compare the printed output text rather than doing a numeric comparison. Under
`%.17g` the printed strings change shape (more digits) although the doubles are
identical, so those goldens fail.

The golden assertions must never be modified, so the only way to keep them
passing is for the CLI default to print the pre-fork `%.6f` form. The three
Netflix CPU golden tests ([ADR-0024](../adr/0024-netflix-golden-preserved.md))
pass bit for bit with this default. See
[ADR-0119](../adr/0119-cli-precision-default-revert.md) for the full rationale.

## Why `%.17g` is the lossless opt-in

IEEE-754 double precision holds about 15.95 significant decimal digits. `%.17g`
is the minimum printf format that guarantees

```text
parse(print(x)) == x    for every finite double x
```

Anything shorter (`%.15g`, `%.6f`) can corrupt scores that differ by one ULP,
which is the resolution at which cross-backend diffs matter. See
[../benchmarks.md](../benchmarks.md) and
[../backends/index.md](../backends/index.md) for the cross-backend ULP budget.

## Related

- [cli.md](cli.md) — full CLI reference, `--precision` summary.
- [ADR-0119](../adr/0119-cli-precision-default-revert.md) — current decision
  (`%.6f` default).
- [ADR-0006](../adr/0006-cli-precision-17g-default.md) — *Superseded.* The
  original `%.17g`-default decision, kept for history.
- [../benchmarks.md](../benchmarks.md) — fork-added benchmark numbers, all
  reported at `--precision=max`.
