---
paths:
  - tools/vmaf-tune/src/vmaftune/compare.py
  - tools/vmaf-tune/tests/test_compare*.py
invariant: compare_codecs_sweep builds one bisect predicate per target; runtime variants are labels, not adapters.
---
<!-- markdownlint-disable MD024 -->
# Codec comparison sweeps

- **`compare_codecs_sweep` builds one bisect predicate per target
  VMAF**, memoised in per-target cache, then flat-dispatches
  cross-product `(codec, target_vmaf)` to thread pool. Do not
  collapse this into single per-codec predicate: bisect closure
  binds `target_vmaf` at construction time (per-iteration
  candidate-CRF probe wants right rung), so re-using one closure
  across multiple targets re-runs same target every time.
- **Dispatch order is ranking input (HISS-04 split).**
  `compare_codecs` and `compare_codecs_sweep` share
  `_dispatch_predicates`, which returns `(index, result)` pairs in
  completion order (submission order when sequential).
  `compare_codecs` feeds that order to `_rank`; its stable sort keeps
  it for equal `(bitrate_kbps, codec)` keys and NaN-bitrate rows. Do
  not sort the dispatcher output by index or route it through a dict.
  Sweep writes rows by index, so order does not matter there. Sweep
  takes `t0` before `_probe_availability`; `wall_time_ms` includes
  probe time.
- **Compare runtime variants are labels, not adapters
  ([ADR-0644](../../../docs/adr/0644-vmaf-tune-codec-runtime-variants.md)).**
  `ADAPTER@VARIANT` tokens in `vmaf-tune compare` must parse through
  `encoder_runtime.resolve_encoder_runtime_specs()`. Base `ADAPTER`
  routes through `codec_adapters.get_adapter()` and
  `probe_encoder_available()`; full token is only display label and
  key for `--encoder-ffmpeg-bin TOKEN=PATH`. Do not add fake
  adapters such as `libsvtav1-hdr` when FFmpeg still exposes
  `-c:v libsvtav1`. Compare JSON/CSV rows must keep `codec` (display
  token), `adapter`, `runtime_variant`, and `ffmpeg_bin` together so
  encoder-profile consumers can audit which runtime produced each
  row.
- **Compare predicate is recommend seam.**
  `compare.compare_codecs` takes
  `predicate(codec, src, target_vmaf) -> RecommendResult`
  callable. Programmatic default predicate returns `ok=False`
  pointing callers at
  `bisect.make_bisect_predicate(target_vmaf, *, width=...,
  height=..., framerate=..., duration_s=...)` because bare
  predicate signature does not carry source geometry.
  `vmaf-tune compare` CLI binds that Phase B
  ([ADR-0326](../../../docs/adr/0326-vmaf-tune-phase-b-bisect.md))
  predicate from its explicit geometry flags by default;
  `--predicate-module MODULE:CALLABLE` hook is only supported way
  to bypass real bisect. `tests/test_compare.py` injects fake
  predicates so ranking is exercised without `ffmpeg` / `vmaf`
  binaries. Do not branch on codec name inside `compare.py` —
  route every per-codec call through predicate / adapter registry.

- **`_run_compare` auto-probes container-source framerate /
  duration (ADR-0509, Bug #V7-1).** When `--src` is container
  (suffix outside `score.VMAF_RAW_SUFFIXES`) and user did NOT
  pass `--framerate` / `--duration` explicitly,
  `_resolve_compare_source_geometry` substitutes probed values
  from `vmaftune.report.probe_source` before building bisect
  predicate. "User passed explicitly" signal rides on
  `_TrackedDefaultAction` + `_stamp_tracked_default_sentinels`
  which set `args._<dest>_was_default = False` for any flag user
  explicitly named. Sentinel is intentionally inverted (default
  `True`, opted-out to `False`) because argparse never invokes
  `Action.__call__` on omitted flags. Explicit user overrides
  win; mismatches emit one-line stderr warning. Regression that
  bypasses helper (i.e. feeds `args.framerate` straight into
  `make_bisect_predicate`) re-opens v7 bug class. Encoder pulls
  frames at container's native rate but `frame_skip_ref` /
  `frame_cnt` walk reference YUV at different rate, collapsing
  VMAF to 4-90 band regardless of CRF. Tests pinning invariant
  live under `tests/test_compare.py` (the
  `test_resolve_compare_source_geometry_*` +
  `test_cli_compare_passes_probed_framerate_for_container_src`
  family); when adding new `_TrackedDefaultAction` flag, extend
  hardcoded tuple in `_stamp_tracked_default_sentinels`.
- **`compare --no-bisect` skips bisect;
  `_run_compare_crf_sweep` owns schema-v3 output (ADR-0542).**
  When `args.no_bisect` is truthy, `_run_compare()` delegates
  immediately to
  `_run_compare_crf_sweep(args, encoders)` — normal bisect path
  is not entered. Sweep function calls `bisect._encode_and_score`
  directly (no iterative search), builds
  `{"schema_version": 3, "mode": "crf_sweep", "rows": [...]}`
  payload, and writes / prints JSON only (CSV / Markdown
  rendering is follow-up). `--target-vmaf` / `--target-vmafs` are
  parsed but act as label-only annotation; they do not influence
  encode loop. Do not short-circuit `_run_compare` before
  format-validation block (format guard still applies). Tests:
  `tests/test_compare_no_bisect.py`.
