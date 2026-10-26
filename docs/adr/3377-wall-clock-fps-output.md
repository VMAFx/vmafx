# ADR-3377: Monotonic Wall-Clock FPS in Report Output

- **Status**: Accepted
- **Date**: 2026-10-26
- **Deciders**: Lusoris
- **Tags**: cli, output, metrics, core

## Context

In libvmaf, the JSON report field `"fps"` and the XML attribute
`<fyi fps="...">` compute throughput with `clock()`: the picture count
divided by `(end - begin) / CLOCKS_PER_SEC`, inherited from Netflix upstream
in `feature_collector.cpp` and `libvmaf.c:output_fps()`.

`clock()` measures the CPU time of all threads of the process, not elapsed
wall time. Multi-threaded CPU runs (such as `--threads 8` on 4K video)
therefore report an `fps` 4-8x below the real throughput (a wall throughput
of 40.0 fps is reported as 5.11 fps). Users read `fps` as real-time
throughput and report performance that is far too low.

## Decision

We add a monotonic wall timer (`wall_timer`) to `VmafFeatureCollector`,
recorded at the same points as the process CPU timer, compute `wall_fps`
beside `output_fps()`, and emit an additive `wall_fps` field in the JSON and
XML reports:

- JSON: new key `"wall_fps": %.2f` directly after `"fps"`, `null` on NaN or
  Inf.
- XML: new attribute `wall_fps="%.2f"` on the existing
  `<fyi fps="..." wall_fps="..." />` element.
- The existing `fps` field stays unchanged for backward compatibility and
  upstream parity (HISS-14 append-only ABI).

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| Redefine `fps` as wall time | No new key | Breaks parsers and upstream parity | Rejected (HISS-14) |
| Additive `wall_fps` | Keeps `fps`; true throughput | One more key and attribute | Chosen |
| Documentation only | No code change | Reports stay misleading | Rejected |

## Consequences

- **Positive**: users get the real throughput (`wall_fps`) on multi-threaded
  and single-threaded runs on every platform.
- **Negative**: each JSON or XML report grows by a few bytes.
- **Neutral / follow-ups**: `docs/usage/cli.md` explains the difference
  between `fps` and `wall_fps`; `core/src/vmafx/report_verify.c` treats
  `wall_fps` as an environment timing field.

## References

- [ADR-0606](0606-macos-vmaf-write-output-segv-deep-fix.md): the guards
  `output_fps()` already has, which `output_wall_fps()` repeats.
- Controller order `vmafx-v1016-3d0h-default-gaps-20261026` (paraphrased: a
  reported 4K throughput drop with `vmaf_v1.0.16_3d0h`; measure, find the
  cause, fix). The measurement showed the report's `fps` counts CPU seconds,
  not wall seconds.
