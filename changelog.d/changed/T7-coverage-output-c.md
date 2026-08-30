- **`core/src/output.c` writer-format coverage 28% → 95% (R3 from
  [`docs/development/coverage-gap-analysis-2026-05-02.md`](docs/development/coverage-gap-analysis-2026-05-02.md)).**
  Adds [`core/test/test_output.c`](core/test/test_output.c) (8 unit
  tests, 230 lines instrumented) exercising the four writer formats
  (XML / JSON / CSV / SUB) end-to-end through `tmpfile()`-backed sinks
  and a synthetic `VmafFeatureCollector`. Branches newly covered: NaN /
  +Inf serialization-as-`null` in JSON frame metrics / pooled scores /
  aggregates / top-level `fps`; XML EINVAL guards on NULL `vmaf` / `fc` /
  `outfile`; `subsample > 1` frame skipping; `count_written_at == 0`
  empty-frame skip; `score_format == NULL` fall-through to
  `DEFAULT_SCORE_FORMAT`; custom `"%.3f"` and `"%.17g"` overriding
  default; multi-aggregate trailing-comma path. No production-code
  changes — pure test-only addition. Headline CPU coverage moves
  ~+0.5 pp toward the 70% ratchet target (per the gap-analysis
  projection).
