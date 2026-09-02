<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1064: Wire score_fmt option on all vmaf FFmpeg filters

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `ffmpeg`, `build`, `api`

## Context

The fork added `vmaf_write_output_with_format()` (commit 695d29626) to expose
the `--precision` flag semantics at the C API level (ADR-0119). This allows
callers to control the printf format string used for score values in log
output — the default `"%.6f"` matches Netflix upstream, while `"%.17g"` gives
IEEE-754 round-trip lossless output.

The FFmpeg filter integration (patches 0001–0015 in `ffmpeg-patches/`) never
adopted this API. All four vmaf filters (`libvmaf`, `libvmaf_sycl`,
`libvmaf_vulkan`, `libvmaf_metal`) still called `vmaf_write_output()`, which
hard-codes `"%.6f"`. Users could not request higher-precision output from
within FFmpeg without post-processing the log file.

An audit of public API additions since the last patch verification (2026-05-20,
ADR-0643) found this gap and one other addition (`vmaf_context_get_backend`,
ADR-0804), which requires no patch update as no filter currently needs it.

## Decision

Add patch `0016-libvmaf-wire-score-fmt-on-all-vmaf-filters.patch` to the
series. The patch:

1. Adds `char *score_fmt` to `LIBVMAFContext`.
2. Exposes `score_fmt` as an `AV_OPT_TYPE_STRING` AVOption (default `NULL`
   = `"%.6f"`) on `libvmaf`, `libvmaf_sycl`, `libvmaf_vulkan`, and
   `libvmaf_metal`.
3. Replaces each `vmaf_write_output()` call with
   `vmaf_write_output_with_format(... s->score_fmt)`.

The pattern is symmetric with `cpumask`/`gpumask` from ADR-0576 (patch 0014).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fold into patch 0001 | Fewer patch files | Breaks the topological ordering; 0001 is a small targeted change; adding struct fields early cascades context changes through 0002–0015 | Not chosen |
| Separate patch per filter | Surgical | 4 patches for one logical change; series grows unnecessarily | Not chosen |
| New patch 0016 (chosen) | All four filters updated atomically; easy to bisect; mirrors ADR-0576 pattern | Series grows to 16 patches | Chosen |

## Consequences

- **Positive**: Users can now write `libvmaf=score_fmt=%.17g` or
  `libvmaf_sycl=log_path=out.xml:score_fmt=%.17g` to get full-precision
  log output from FFmpeg, matching `--precision=max` on the libvmaf CLI.
- **Negative**: Patch 0016 has not yet been replay-verified against pristine
  n8.1.1; a `git am --3way` series run is required before merge.
- **Neutral**: The `score_fmt` option accepts any string; invalid printf
  formats produce undefined output per the `vmaf_write_output_with_format()`
  API contract.

## References

- ADR-0119: `--precision` flag and `vmaf_write_output_with_format()` addition
- ADR-0576: cpumask/gpumask exposure pattern (patch 0014)
- ADR-0643: last full series verification (2026-05-20, 15 patches against n8.1.1)
- ADR-0804: `vmaf_context_get_backend()` addition (not requiring patch update)
- commit 695d29626: initial addition of `vmaf_write_output_with_format()` to
  `core/include/libvmaf/libvmaf.h`
