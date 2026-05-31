<!-- markdownlint-disable MD013 -->
# Research-0090: Phase-A-promotion audit — 2026-05-08

- **Status**: Active
- **Workstream**: ADR-0237 (vmaf-tune), ADR-0209 (embedded MCP),
  ADR-0261 (HDR-aware), ADR-0276 (fast path), ADR-0288 (codec
  adapters), ADR-0307 (ladder default sampler), ADR-0288 (vmaf-roi
  Option C), T7-10b (HIP runtime), T5-2b (MCP runtime), T6-2c
  (vmaf-roi mask materialisation)
- **Last updated**: 2026-05-08
- **Author**: Phase-A-promotion audit subagent

## TL;DR

Total scaffold / Phase-A markers found across fork-local code: **38
distinct surfaces** (after de-duplicating multi-line docstring hits
and discarding upstream-Netflix abstract-method idioms in
`python/vmaf/core/`).

- **Production-blocking (recommended next sprint): 5.** Two of these
  are *silent* — code paths that look wired but quietly fall through
  to a no-op or `NotImplementedError` at runtime.
- **Cosmetic / docs-only drift: 12.** Mostly `tools/vmaf-tune/`
  module docstrings, README, and `docs/usage/vmaf-tune.md` framing
  the surface as "Phase A only" when 6 phases of CLI subcommands and
  17 codec adapters have shipped.
- **Already resolved by a sibling agent / merged PR (skip-removable
  or status-flippable): 12.** 9 of these are `Status: Proposed` ADRs
  whose code has fully shipped (ADR-0125, 0127, 0128, 0129, 0138,
  0139, 0140, 0270, 0295) — a Proposed→Accepted sweep is mechanical.

The most surprising single finding: **`vmaf-tune` does not actually
dispatch through `CodecAdapter.ffmpeg_codec_args(...)` for live
encodes.** `corpus.iter_rows` and `per_shot._segment_command` both
hardcode `-c:v <encoder> -preset <p> -crf <q>`, so 15 of the 17
registered adapters (everything except `libx264`/`libx265`) are
non-functional for live grids — `libaom-av1` would actually fail
ffmpeg invocation because libaom uses `-cpu-used`, not `-preset`.
The adapters' `ffmpeg_codec_args(...)` method is only exercised by
the predictor probe-encode path (`_gop_common.probe_args`), not the
Phase A grid sweep that the docstrings describe as the canonical
corpus generator. ADR-0237's "codec-adapter contract is multi-codec
from day one" invariant is currently a docstring-only contract.

## Method

Repo-wide grep for `Phase A only|Phase A scaffold|Phase B
pending|scaffold-only|stub-only`, then for `raise
NotImplementedError|FIXME.*ADR-[0-9]+|TODO.*Phase [BCDEF]`.
Cross-referenced ADR `Status:` lines against shipped code under
`core/src/`, `tools/`, `ai/`. Verified each scaffold marker by
following the import graph from the CLI / public-API entry point to
the seam that should be wired. Sibling-agent overlap is flagged
explicitly per the spec.

## High-priority promotions (production-blocking)

### HP-1. `corpus.iter_rows` / `per_shot._segment_command` bypass `CodecAdapter.ffmpeg_codec_args()`

- **Files**:
  `tools/vmaf-tune/src/vmaftune/encode.py:97-105`,
  `tools/vmaf-tune/src/vmaftune/corpus.py:154-178`,
  `tools/vmaf-tune/src/vmaftune/per_shot.py:330-364`.
- **Current state**: `encode.build_ffmpeg_command(...)` hardcodes
  `["-c:v", req.encoder, "-preset", req.preset, "-crf",
  str(req.crf)]`. `corpus.iter_rows` validates the request through
  the adapter (`adapter.validate(...)`) but then constructs an
  `EncodeRequest` and never calls `adapter.ffmpeg_codec_args(...)`.
  `per_shot._segment_command` likewise bakes
  `("-c:v", encoder, "-crf", str(crf))` directly into the segment
  argv.
- **Why this is production-blocking**: the registry advertises 17
  codecs and the CLI accepts all of them via
  `choices=list(known_codecs())`. For 15 of them the resulting
  argv is wrong:
  - `libaom-av1` requires `-cpu-used N` instead of `-preset NAME`;
    the current path emits `-preset medium` to libaom which ffmpeg
    accepts but libaom ignores, so the speed/quality knob is
    pinned at libaom's default regardless of the harness preset.
  - NVENC / AMF / QSV / VideoToolbox families need extra rate-control
    flags (`-rc constqp`, `-cq`, `-global_quality`, etc.) that the
    adapter's `ffmpeg_codec_args` returns and the encode path
    discards.
  - `libvvenc` accepts `-preset` and a quality knob but only via
    `-qp`; `-crf` is silently demoted to default.
- **What's blocking promotion**: `EncodeRequest` is shaped around the
  x264/x265 `(preset, crf)` pair. Promoting requires (a) extending
  `EncodeRequest` with an `extra_codec_args: tuple[str, ...]` slot
  populated by `adapter.ffmpeg_codec_args(preset, crf)`, (b) dropping
  the hardcoded `-preset / -crf` slice from
  `build_ffmpeg_command`, (c) the same edit on
  `per_shot._segment_command`, (d) updating the test fakes that
  capture `cmd` to assert against the adapter-emitted argv.
- **Effort**: medium (200–400 LOC including test rework).
  ~1-day PR on its own. Risk: needs a manual smoke against at
  least one non-x264 adapter before merging because the existing
  unit tests mock `subprocess.run` and have never exercised real
  ffmpeg with non-x264 argv.

### HP-2. HDR-aware encoding ships CLI surface but iter_rows ignores it

- **Files**:
  `tools/vmaf-tune/src/vmaftune/cli.py:141-176, 621`,
  `tools/vmaf-tune/src/vmaftune/corpus.py:91-93`,
  `tools/vmaf-tune/src/vmaftune/hdr.py` (whole module),
  `tools/vmaf-tune/tests/test_hdr.py:295-353`.
- **Current state**: PR #434 / ADR-0261 advertises "HDR-aware
  encoding + HDR-VMAF scoring". The CLI exposes `--auto-hdr` /
  `--force-sdr` / `--force-hdr-pq` / `--force-hdr-hlg`, plumbs
  `hdr_mode` onto `CorpusOptions`, and `hdr.py` provides
  `detect_hdr` / `hdr_codec_args` / `select_hdr_vmaf_model`.
  However, **no module imports `vmaftune.hdr`** —
  `grep -nE "import.*hdr|from.*\.hdr" tools/vmaf-tune/src/vmaftune/*.py`
  returns zero hits. The integration tests
  (`test_corpus_emits_hdr_fields_when_source_is_hdr`,
  `test_corpus_force_sdr_skips_hdr_path`) are gated behind
  `_HDR_ITER_ROWS_DEFERRED = pytest.mark.skip(reason="iter_rows
  HDR integration deferred to follow-up; only CLI surface in this
  PR")`.
- **Why this is production-blocking**: the user-facing surface (CLI
  flags, ADR documentation) advertises HDR support that does not
  exist. An operator running
  `vmaf-tune corpus --auto-hdr ...` against a real HDR PQ source
  silently gets an SDR encode with PQ-tagged metadata stripped —
  the exact SDR-as-HDR misclassification failure mode the
  `hdr.py` docstring (line 19-24) calls "the dangerous failure
  mode".
- **What's blocking promotion**: roughly 50 LOC inside
  `corpus.iter_rows` — call `detect_hdr(job.source)` at the top of
  the loop, fold the result into `EncodeRequest.extra_params` via
  `hdr_codec_args(adapter.encoder, info)`, and route the model
  selection to `select_hdr_vmaf_model(...)` when score requests are
  built. The deferred tests already specify the exact contract the
  patch needs to satisfy.
- **Effort**: small (~100 LOC + un-skip 2 tests).
  Half-day PR. Same PR removes `_HDR_ITER_ROWS_DEFERRED`.

### HP-3. `vmaf-tune fast` production wiring is Python-only and still crashes on missing seams

- **Files**:
  `tools/vmaf-tune/src/vmaftune/fast.py:200-219, 246-302, 438-468`,
  `tools/vmaf-tune/src/vmaftune/cli.py` (no `fast` subparser).
- **Current state**: `changelog.d/added/vmaf-tune-fast-path-prod-wiring.md`
  claims `vmaf-tune fast` "graduates from scaffold-only to
  production-wired", but:
  - There is **no `fast` subcommand** in
    `cli.py:_build_parser` — only `corpus`, `recommend`, `predict`,
    `tune-per-shot`, `recommend-saliency`, `ladder`, `compare`.
    `fast_recommend()` is reachable only via a direct Python API
    call.
  - The production path inside `fast_recommend` raises
    `NotImplementedError` from `_build_prod_predictor` (line 213-219:
    "vmaf-tune fast production predictor needs a sample_extractor
    callable…") and from `_gpu_verify` (line 291-296: "vmaf-tune
    fast verify pass needs an encode_runner callable…") unless the
    caller injects both seams.
  - The CLI never injects either seam, and even if a `fast`
    subcommand existed, the changelog promise of
    "Optuna TPE search drives a proxy-backed CRF→VMAF predictor"
    is contingent on a same-PR follow-up that wires the existing
    `vmaftune.encode` + score pipeline to those seams.
- **Why this is production-blocking**: the changelog and ADR-0276
  / ADR-0304 documentation tell users `vmaf-tune fast` is
  production-ready. In practice, every documented invocation hits
  one of the two `NotImplementedError` paths before any GPU verify
  runs.
- **What's blocking promotion**: (a) add the `fast` subparser to
  `cli.py` (~50 LOC argparse + a `_run_fast` dispatcher),
  (b) write the production `sample_extractor` and `encode_runner`
  closures by composing the existing `vmaftune.encode.run_encode` +
  `vmaftune.score.run_score` + `vmaftune.predictor_features.extract_features`
  pipelines (~150 LOC), (c) un-skip / add the integration test
  that drives the full CLI path. The seam contracts in `fast.py`
  are clear and the underlying primitives all exist.
- **Effort**: medium (~250 LOC, ~1 day). The work is purely glue —
  every primitive it needs ships in a sibling module already.

### HP-4. Embedded MCP scaffold (`core/src/mcp/mcp.c`) returns -ENOSYS unconditionally

- **Files**:
  `core/src/mcp/mcp.c:1-20` (header docstring + every entry
  point), `docs/mcp/embedded.md:27`, `docs/mcp/index.md:11`,
  ADR-0209 / ADR-0128 (Status: Proposed since 2026-04-27).
- **Current state**: every public entry point in the embedded MCP
  TU returns `-ENOSYS` or a trivially-safe constant. The runtime
  (cJSON + mongoose vendoring, dedicated MCP pthread, SPSC ring
  buffer drained at frame boundaries, SSE / UDS / stdio transport
  bodies) is gated behind unscheduled task `T5-2b`. The docs page
  for the embedded MCP surface tells users it is "scaffold-only —
  the runtime arrives in T5-2b".
- **Why this is production-blocking only by ambition**: the user
  said "we have most prep or research stuff done" — the MCP runtime
  is the largest piece of fork-local prep that has not yet
  graduated. ADR-0128 / ADR-0209 + Research-0005 cover the
  decisions; the smoke test (`test_mcp_smoke.c`) pins the
  `-ENOSYS` contract precisely so that the runtime PR can flip
  the matrix in one place.
- **What's blocking promotion**: substantial. Vendoring choices
  (cJSON via subproject, mongoose via subproject), an MCP pthread
  with a bounded SPSC drain at frame boundaries, three transport
  bodies (stdio, UDS, SSE), and Power-of-10 alloc/loop discipline
  on the measurement-thread hot path. ADR-0209 §"What lands next"
  is a roadmap, not a punch list.
- **Effort**: large (1500–3000 LOC + 200 LOC tests + Meson
  subproject wiring + a CI matrix flip). Likely a 2-week PR even
  for a single-transport (stdio-only) first cut.

### HP-5. HIP feature kernels return -ENOSYS pending T7-10b runtime

- **Files**: `core/src/feature/hip/*.c` (every TU),
  `core/src/feature/feature_extractor.c:211-217` (registry-side
  scaffold note), `docs/backends/hip/overview.md:216` (`scaffold-only
  audit-first PR` framing), `core/src/hip/AGENTS.md:245`.
- **Current state**: every HIP feature extractor — `adm_hip.c`,
  `float_ssim_hip.c`, `float_psnr_hip.c`, `float_moment_hip.c`,
  `integer_psnr_hip.c` — returns `-ENOSYS` from `init` and `extract`,
  with a multi-paragraph block comment explaining that "the
  runtime PR (T7-10b) will swap these for real device-buffer
  handles". The feature-registry hook in
  `feature_extractor.c:211-217` documents the same scaffold-only
  contract.
- **Why this is production-blocking only by ambition**: AMD users
  have no HIP path today; the ROCm 7.x toolchain is shipped, the
  CUDA twin's algorithms are bit-exact-tested, and the kernel
  templates compile. The runtime PR is a known, scoped piece of
  work.
- **What's blocking promotion**: a HIP buffer-alloc helper, a
  per-feature `submit` body, score emission, and a CI matrix
  entry. Mirrors the CUDA twin's ~600 LOC per feature × 5 features.
- **Effort**: large (3000–5000 LOC for full parity; 800–1500 LOC
  for a single-feature first cut to prove the runtime, then
  per-feature follow-ups).

## Cosmetic / docs-only drift

These are one-line / one-paragraph fixes; bundle them into a single
PR or roll them into the docs cleanup sibling agent's queue.

- `tools/vmaf-tune/__init__.py:3` — module docstring still reads
  "vmaf-tune — quality-aware encode automation harness (Phase A)"
  when 7 subcommands and 17 codec adapters ship. Drop the
  "(Phase A)" qualifier; rewrite the second paragraph to describe
  the current shipped scope.
- `tools/vmaf-tune/README.md:1, 5-13, 29` — title is "vmaf-tune
  (Phase A scaffold)"; "Phase A scope (this PR): drives
  `ffmpeg`/`libx264` over a parameter grid"; layout block claims
  the only adapter is `x264.py` with "(Phase A only)"; "Phase B
  (target-VMAF bisect) and Phase C (per-title CRF predictor) are
  NOT implemented here". Phases C, D, E shipped; only Phase B
  bisect is genuinely outstanding. Already noted by Research-0088;
  flagging here for cross-reference.
- `tools/vmaf-tune/AGENTS.md:11-12, 25, 34, 58, 104-105` —
  describes the corpus row schema, codec-adapter rule, and
  fast-path policy as "Phase A" / "Phase B/C/D will consume".
  Update to current vocabulary now that B-lite, C, D, E ship.
- `docs/usage/vmaf-tune.md:25-39, 1229` — top-of-file framing
  ("This doc covers Phase A of the six-phase roadmap"; "Phase E is
  currently scaffold-only"; "Codecs wired so far: libx264 (Phase A
  scaffold) and libx265"). Phase E was wired by ADR-0307; the
  codec list is one of 17. Already in Research-0088's queue.
- `tools/vmaf-tune/src/vmaftune/cli.py:5-7, 36, 44, 66, 195,
  275, 299, 411, 467, 1085-1090, 1118-1126` — argparse `help=`
  strings and module docstring all framed as "Phase A drives a
  …libx264… grid sweep". Help strings on `--encoder` claim
  "Phase A: libx264 only" but the choices list is the full registry.
- `tools/vmaf-tune/src/vmaftune/recommend.py:5,
  resolution.py:16-34, codec_adapters/vvenc.py:12-39,
  codec_adapters/_amf_common.py:21-186,
  codec_adapters/libaom.py:3-25, codec_adapters/svtav1.py:3-145,
  codec_adapters/_nvenc_common.py:29, codec_adapters/x265.py:7-78,
  codec_adapters/_qsv_common.py:105, codec_adapters/x264.py:3-77,
  codec_adapters/_videotoolbox_common.py:69` — each says some
  variant of "Phase A wires X; Phase B+ extends" or "outside Phase
  A scope". Many of those Phase B/C/D/E references have shipped.
- `tools/vmaf-tune/src/vmaftune/encode.py:3,
  score.py:3, 122, fast.py:3, 27, 72, 204, 480` —
  "ffmpeg/libx264 driver — Phase A", "vmaf binary driver — Phase
  A", "Phase A grid (ADR-0276 fallback contract)". The grid is
  no longer the sole driver; ladder + per-shot + saliency-roi sit
  alongside.
- `tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py:8-25` —
  the docstring "Phase A wires libx264 plus the NVIDIA NVENC
  family … the AMD AMF family …" is technically accurate as a list
  of registered adapters but conflates "registry entry exists" with
  "live encode works", which is exactly the HP-1 drift above.
  Either accurate the docstring (HP-1 fix) or rephrase as
  "metadata-registered, encode-pending" until HP-1 lands.
- `tools/vmaf-tune/src/vmaftune/saliency.py:26` — "only two
  encoders Phase A / Bucket #2 need". Saliency-ROI extension
  sibling agent is in flight; coordinate.
- `tools/vmaf-tune/src/vmaftune/codec_adapters/libaom.py:22-25` —
  "Phase A wires this adapter's metadata only — `encode.py` is
  not yet codec-pluggable for non-`-preset` encoders, so live
  grid sweeps with libaom unblock once that lands". This is the
  one place in-tree that *correctly* documents HP-1; it is
  deceptive primarily because every other adapter docstring claims
  to be wired.
- `ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch:14`
  — "supersedes the scaffold-only declaration that PR #111
  (ADR-…) shipped". Stale framing in a patch header that has
  since been overtaken by the Vulkan kernel ports.
- `docs/backends/vulkan/overview.md:279`,
  `core/src/vulkan/AGENTS.md:139` — "scaffold-only audit-first
  PR (T5-1)" framing in pages that now describe a Vulkan backend
  with 15+ feature kernels. Status-flip alongside ADR-0127.

## Skip markers worth removing

- `tools/vmaf-tune/tests/test_hdr.py:295-353` — `_HDR_ITER_ROWS_DEFERRED`
  block (two tests). Removable as soon as HP-2 lands; the tests
  are pre-written and pin the exact contract.
- `tools/vmaf-tune/tests/test_compare.py:249-258` — `pytest.mark.skip`
  on `test_cli_compare_stdout_smoke` because `--predicate-module`
  is "not wired in the minimal `compare` subcommand shipped here;
  predicate-module dynamic-import lands in the Phase B bisect
  follow-up". Owned by the in-flight Phase B bisect sibling agent;
  flag here for tracking only.
- `python/test/result_test.py:206`, `python/test/routine_test.py:398`,
  `python/test/feature_extractor_test.py:344`,
  `python/test/quality_runner_test.py:1557` — upstream-Netflix
  skips ("numerical value has changed", "Inconsistent numerical
  values", "vifdiff alternative needed"). Out of fork scope per
  CLAUDE.md §8 (do not touch Netflix golden-data assertions); list
  here only so the audit is complete.
- `python/test/perf_metric_test.py:99,116,133`,
  `python/test/cross_validation_test.py:130`,
  `python/test/model_registry_schema_test.py:134`,
  `ai/tests/test_ptq_scripts.py:63`,
  `tools/vmaf-tune/tests/test_codec_adapter_x265.py:309` — all
  conditional `skipIf` based on optional dependency presence
  (sklearn, sklearn-version, onnxruntime, ffmpeg-binary). Working
  as intended.

## ADR status flips

These ADRs sit at `Status: Proposed` despite shipped-and-tested
code on master. A mechanical sweep PR can flip each one to
`Accepted` with a one-line "Implementation: shipped in PR #NNN" pin
in the Decision section.

- **ADR-0125 (`ms-ssim-decimate-simd`)** — AVX2 +
  AVX-512 implementations under
  `core/src/feature/x86/ms_ssim_decimate_avx2.c` /
  `..._avx512.c` ship and are bit-exact-tested. ADR header says
  "Proposed (amended 2026-04-20 — separable-form chosen with…".
- **ADR-0127 (`vulkan-compute-backend`)** — 15+ feature kernels
  under `core/src/feature/vulkan/`,
  `core/src/vulkan/{import,picture_vulkan,dispatch_strategy}.c`,
  GPU-parity CI gate (T6-8 / ADR-0214) green for all but the
  ciede precision gap (Research-0055).
- **ADR-0128 (`embedded-mcp-in-libvmaf`)** — *do not flip*. Still
  scaffold-only (HP-4). Listed for completeness because the
  shipped scaffold + `enable_mcp` Meson option might tempt a
  reviewer to flip it.
- **ADR-0129 (`tinyai-ptq-quantization`)** — int8 PTQ shipped via
  `ai/scripts/quantize_int8.py` and `model/*_int8.onnx`; calibration
  accuracy targets met (Research-0006). Flippable.
- **ADR-0138 (`iqa-convolve-avx2-bitexact-double`)** — shipped.
  Bit-exactness gate green (Research-0011).
- **ADR-0139 (`ssim-simd-bitexact-double`)** — shipped
  (Research-0012).
- **ADR-0140 (`simd-dx-framework`)** — shipped, lifted across
  AVX2 / AVX-512 / NEON. T7-5 closeout (ADR-0278) cites this as a
  load-bearing invariant, which presumes Accepted status.
- **ADR-0270 (`fuzzing-scaffold`)** — fuzzing scaffold shipped;
  Research-0083 expanded the target survey. Flip after confirming
  it's not meaningfully different from "scaffold + research"
  status.
- **ADR-0295 (`vmaf-tune-phase-e-bitrate-ladder`)** — shipped
  (PR #433); ADR-0307 layers the default-sampler decision on top.
  The default sampler shipped (`ladder._default_sampler`) so the
  full design is live.
- **ADR-0276 (`vmaf-tune-fast-path`)** — *do not flip yet*; HP-3
  blocks. The fast-path Python API is wired but the production
  CLI promised by the changelog is missing.
- **ADR-0237 (`quality-aware-encode-automation`)** — Status reads
  "Accepted (Phase A only; Phases B–F remain Proposed)". This
  line is the canonical phase-A drift marker — it has not been
  updated despite Phases C, D, E shipping. Suggested rewrite:
  "Accepted; Phases A/C/D/E shipped (PRs #NNN/#NNN/#NNN/#NNN);
  Phase B bisect outstanding (sibling Phase B agent);
  Phase F MCP outstanding (HP-4)".
- **ADR-0296 (`vmaf-roi-saliency-weighted`)** — Status reads
  "Accepted (Option C scaffold only; Option A remains Proposed)".
  T6-2c (Option C mask materialisation) still pending —
  `mask.apply_saliency_mask` raises `NotImplementedError` at line
  78 and the inference closure raises at line 128. Do not flip
  Option A; flag here as a low-effort follow-up because the
  saliency-roi sibling agent is in flight and may already be on
  this.

## Recommended sprint plan

Ranked by leverage (user-visible impact ÷ engineering effort).

1. **HP-2: HDR integration into `iter_rows`** (small, ~100 LOC).
    The biggest "appears-shipped-but-isn't" bug. Operator-facing
    silent failure with a documented dangerous-failure-mode
    classification. One-day PR; un-skips two pre-written tests.
2. **Cosmetic-drift sweep** (medium, ~30 file touches, no
    behavioural change). Bundle the 12 docs/docstring fixes
    above into one PR. Drains the "Phase A scaffold" framing
    that mis-sells the harness to users. Coordinate with the
    docs-cleanup sibling agent (Research-0088) — likely overlap.
3. **HP-1: codec-adapter `ffmpeg_codec_args` dispatch** (medium,
    200–400 LOC). The biggest *correctness* gap in vmaf-tune.
    Without this, ADR-0237's "multi-codec from day one" invariant
    is a docstring lie. One-day PR + one manual smoke encode per
    adapter family.
4. **ADR Proposed→Accepted sweep** (small, mechanical).
    9 flippable ADRs (0125, 0127, 0129, 0138, 0139, 0140, 0270,
    0295, 0237 phase-line update). Single doc-only PR; reviewer
    cost is low because each line cites a shipped PR. Trade a
    1-hour edit for cleaner ADR-status reporting in every future
    audit.
5. **HP-3: `vmaf-tune fast` CLI subcommand + production runners**
    (medium, ~250 LOC). Closes the gap between the changelog
    claim ("graduates from scaffold-only to production-wired")
    and the actual surface (Python API only, both seams unwired).
    One-day PR; the underlying primitives all exist.

HP-4 (embedded MCP) and HP-5 (HIP) are large and out of sprint
scope; they belong on a separate roadmap. Both are well-documented
in their respective ADRs and would each warrant their own dossier.

## Open questions

- Does the saliency-ROI codec-extension sibling agent's plan
  include a fix for the
  `vmaf-roi-score/src/vmafroiscore/mask.py` `T6-2c` deferrals?
  If yes, drop those from this audit. If no, they belong on the
  next sprint.
- The Phase B bisect sibling agent (`ae2a0ec5891a8b7dd`)
  presumably ships the `compare._default_predicate` replacement.
  Confirm the agent's output also un-skips
  `tools/vmaf-tune/tests/test_compare.py:249-258` so it doesn't
  fall through this audit's net.
- Should the `vmaf-tune fast` CLI subcommand land *before* a
  proxy-vs-verify diagnostic surface is added? The changelog
  framing ("flagged OOD when gap > tolerance") implies operator
  visibility into when to fall back, but no log / report path
  surfaces this today.

## Sibling-agent scope coordination

- Phase B bisect agent — owns
  `compare._default_predicate` replacement (HP-? not in this
  list) and the `--predicate-module` skip in `test_compare.py`.
- Docs cleanup agent (Research-0088 punch list) — owns most of
  the cosmetic drift section above.
- Phase F design agent — owns the MCP / composition surface; HP-4
  may or may not be in their scope.
- Tiny-AI SOTA web research agent — orthogonal.
- Upstream-SKIP triage agent — orthogonal (covers upstream
  cherry-pick gaps, not fork-internal scaffolds).
- State.md audit agent — overlaps on the
  ADR-Proposed-but-shipped findings; cross-link.
- Saliency video-temporal + saliency ROI codec-extension agents
  — overlap on `saliency.py:26` framing and possibly on
  `vmaf-roi-score/mask.py` T6-2c. Coordinate before any sweep PR
  touches saliency surfaces.

## References

- ADR-0237 (`docs/adr/0237-quality-aware-encode-automation.md`) —
  the master phase-A→F roadmap.
- ADR-0261 (`docs/adr/0261-vmaf-tune-hdr-aware.md`) — HDR-aware
  encoding decision (HP-2).
- ADR-0276 (`docs/adr/0276-vmaf-tune-fast-path.md`) +
  ADR-0304 (fast-path production wiring) — HP-3.
- ADR-0288 (`docs/adr/0288-vmaf-tune-codec-adapter-x265.md`) —
  the multi-codec adapter contract HP-1 violates.
- ADR-0295 / ADR-0307 — Phase E ladder + default sampler.
- ADR-0209 / ADR-0128 — embedded MCP runtime (HP-4 / T5-2b).
- T7-10b — HIP runtime (HP-5).
- T6-2c — vmaf-roi-score mask materialisation.
- Research-0044 — vmaf-tune option-space digest (origin of the
  "Phase A first" framing).
- Research-0079 — vmaf-tune ladder default-sampler (HP-2 sibling
  context).
- Research-0083 — libfuzzer harness expansion target survey
  (ADR-0270 status-flip evidence).
- Research-0088 — docs cleanup punch list (cosmetic drift sibling
  scope).
- `changelog.d/added/vmaf-tune-fast-path-prod-wiring.md` — the
  "production-wired" claim HP-3 contradicts.
- `tools/vmaf-tune/AGENTS.md` — codec-adapter contract and the
  Phase B/C/D consumer expectations that frame HP-1 as a
  contract violation.
