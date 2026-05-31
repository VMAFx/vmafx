# `--precision` — score output precision

`--precision` controls how VMAF scores are formatted in XML / JSON / CSV / SUB
output and on stderr. Fork-added per
[ADR-0119](../adr/0119-cli-precision-default-revert.md) (which supersedes
[ADR-0006](../adr/0006-cli-precision-17g-default.md)). The default matches
upstream Netflix `%.6f` exactly so the CPU golden gate
([CLAUDE.md §8](../../CLAUDE.md)) passes without explicit flags. Round-trip
lossless `%.17g` is opt-in via `--precision=max`.

## Grammar

```text
--precision N          # integer 1..17 → printf "%.<N>g"
--precision max        # alias for "%.17g" (IEEE-754 round-trip lossless)
--precision full       # alias for "%.17g"
--precision legacy     # "%.6f" — synonym for the default (pre-fork format)
```

Defaults (no `--precision`): `%.6f`.

## When to pick each

| Mode | Use when |
| --- | --- |
| no flag / `legacy` | **Default, Netflix-compatible.** Matches upstream byte-for-byte. Required for the golden gate, FFmpeg `vf_libvmaf`, and any consumer that parses the existing CLI output schema. |
| `max` / `full` | Cross-backend numeric diff (CPU vs SIMD vs CUDA vs SYCL), archival reports where every ULP matters, or any pipeline that re-parses scores into doubles and compares them. |
| `N = 3` to `N = 6` | Human reading on a terminal where you want shorter scores. Do not pipe this into a tool that compares scores. |
| `N = 7` to `N = 16` | Intermediate — no common reason; usually you want `max`. |
| `N = 17` | Equivalent to `max` explicitly. |

## Why `%.6f` is the default

Several Netflix golden tests (e.g.
[`python/test/command_line_test.py`](../../python/test/command_line_test.py))
do **exact-string** match against XML output, not `assertAlmostEqual`. Under
`%.17g` the printed strings change shape (more digits) even though the
underlying doubles are identical, so the goldens fail. The only way to keep
those assertions passing without modifying them — which CLAUDE.md §8 forbids
— is for the CLI's default to print the pre-fork `%.6f` form. See
[ADR-0119](../adr/0119-cli-precision-default-revert.md) for the full
rationale.

## Why `%.17g` is the lossless opt-in

IEEE-754 double precision holds ~15.95 significant decimal digits. `%.17g` is
the *minimum* printf format that guarantees:

```text
parse(print(x)) == x    for every finite double x
```

Anything shorter (`%.15g`, `%.6f`) can round-trip-corrupt scores that differ by
one ULP — exactly the resolution at which cross-backend diffs matter. See
[../benchmarks.md](../benchmarks.md) and
[../backends/index.md](../backends/index.md) for the cross-backend ULP
budget. Pass `--precision=max` whenever you intend to compare scores
numerically across backends.

## Effect on all output channels

The same format applies uniformly to:

- Pooled stderr line (`vmaf_v0.6.1: 76.668905`)
- XML per-frame attribute values
- JSON per-frame + pooled numbers
- CSV cells
- SubRip `<subtitle>` text

There is no way to pick different precisions for different outputs in one
invocation — this is intentional so that `output.xml`, `output.json`, and the
stderr line always agree.

## Example — round-trip check

```shell
./build/core/tools/vmaf \
  --reference  src01_hrc00_576x324.yuv \
  --distorted  src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --output scores.json --json
```

Default output (Netflix-compatible):

```json
{"vmaf": {"mean": 76.668905, ...}}
```

Same run with `--precision max`:

```json
{"vmaf": {"mean": 76.66890501970558, ...}}
```

The default form drops ~9 significant digits. Across the 3 Netflix CPU golden
pairs, the default and `max` form agree to 6 decimals by construction —
but that is the exact margin at which SIMD / GPU backends deviate, so
archival reports and cross-backend diffs must use `--precision=max`.

## Interaction with goldens

The 3 Netflix CPU golden tests
([ADR-0024](../adr/0024-netflix-golden-preserved.md)) pass bit-for-bit with
the `%.6f` default — including the exact-string XML assertions in
`python/test/command_line_test.py` that originally drove this revert.

## Related

- [cli.md](cli.md) — full CLI reference, `--precision` summary row.
- [ADR-0119](../adr/0119-cli-precision-default-revert.md) — current decision
  (`%.6f` default).
- [ADR-0006](../adr/0006-cli-precision-17g-default.md) — *Superseded.*
  Original `%.17g`-default decision; kept for history.
- [../benchmarks.md](../benchmarks.md) — fork-added benchmark numbers, all
  reported at `--precision=max`.
