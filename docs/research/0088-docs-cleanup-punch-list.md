<!-- markdownlint-disable MD013 MD025 -->
# Docs cleanup punch list (2026-05-08)

# Research-0088

Scope: `docs/{usage,api,metrics,backends,development,ai,mcp,architecture}/`,
`docs/state.md`, `docs/rebase-notes.md`, `README.md`, `CHANGELOG.md`,
`tools/*/README.md`. ADRs themselves and `docs/research/` excluded.

92 markdown files scanned; 57 broken-internal links + 2 dead ADR slugs
detected mechanically; remaining items found by spot-reading.

## Broken internal links (57 total)

### docs/ tree

- `docs/usage/python.md:331` — `../../resource/images/scatter_training.png`
  resolves to `resource/images/scatter_training.png` (does not exist;
  `resource/` directory was never added to fork). Likely the upstream
  Netflix tree had it; either copy the asset under `docs/` or strip the
  embed.
- `docs/usage/python.md:332` — same problem with
  `../../resource/images/scatter_testing.png`.
- `docs/usage/vmaf-perShot.md:127` — links
  `../adr/0220-transnet-v2-shot-detector.md`. Real file is
  `docs/adr/0223-transnet-v2-shot-detector.md`. ADR-0220 on disk is
  `0220-sycl-fp64-fallback.md` (unrelated). Slug-drift: rewrite to
  `0223-…`.
- `docs/usage/vmaf-perShot.md:167` — same wrong target
  (`0220-transnet-v2-shot-detector.md` → `0223-…`).
- `docs/usage/vmaf-roi.md:5` — `../metrics/mobilesal.md` does not
  exist. `docs/metrics/` has no per-feature MobileSAL doc; either create
  it (see *Missing surface docs* below) or reroute to
  `docs/ai/models/mobilesal.md`.
- `docs/api/index.md:95` — `../adr/0154-score-pooled-eagain.md` →
  actual slug is `0154-score-pooled-eagain-netflix-755.md`.
- `docs/api/index.md:191` — `../adr/0152-monotonic-index-rejection.md` →
  actual slug is `0152-vmaf-read-pictures-monotonic-index.md`.
- `docs/api/gpu.md:73` — `../adr/0157-cuda-state-free-api.md` → actual
  slug is `0157-cuda-preallocation-leak-netflix-1300.md`.
- `docs/api/gpu.md:393` — `../adr/0127-vulkan-backend-decision.md` →
  actual slug is `0127-vulkan-compute-backend.md`.
- `docs/metrics/confidence-interval.md:67` —
  `../../resource/images/CI.png` does not exist (same upstream-asset
  drift as `usage/python.md`).
- `docs/metrics/features.md:501` — `../adr/0153-float-ms-ssim-min-size.md` →
  actual slug is `0153-float-ms-ssim-min-dim-netflix-1414.md`.
- `docs/metrics/features.md:67` — narrative `ADR-0268` mention; no
  ADR with that number exists. Likely wrong number or stale plan
  reference (the immediately-following text discusses lpips_sq /
  fastdvdnet so check if the intended ADR is `0041-lpips-sq-extractor.md`
  or `0250-tiny-ai-extractor-template.md`).
- `docs/backends/cuda/overview.md:125` —
  `../../../core/src/cuda/ring_buffer.c` does not exist (file was
  renamed/removed; only `core/src/cuda/common.c` etc. ship today).
- `docs/backends/nvtx/profiling.md:22` — same dead
  `core/src/cuda/ring_buffer.c` reference.
- `docs/backends/vulkan/overview.md:24` and `:276` —
  `../../adr/0127-vulkan-backend-decision.md` → real slug
  `0127-vulkan-compute-backend.md` (same drift as `api/gpu.md`).
- `docs/development/oneapi-install.md:216` —
  `../adr/0127-vif-as-sycl-pathfinder.md` does not exist; ADR-0127
  on disk is the Vulkan-backend ADR. Either the intended ADR has a
  different number, or the link is to a planning-dossier document
  that never landed.
- `docs/development/build-flags.md:47` — `../adr/0270-libfuzzer-y4m-input.md` →
  real slug is `0270-fuzzing-scaffold.md`.
- `docs/ai/quantization.md:11` — `../adr/0129-tinyai-ptq-int8-modes.md` →
  real slug is `0129-tinyai-ptq-quantization.md`.
- `docs/ai/bvi-dvc-corpus-ingestion.md:125` and `:135` —
  `../adr/0303-fr-regressor-v2-ensemble-flip.md` → real slug is
  `0303-fr-regressor-v2-ensemble-prod-flip.md`.
- `docs/ai/models/fr_regressor_v3.md:28` —
  `../../adr/0040-tinyai-loader.md` → real slug is
  `0040-dnn-session-multi-input-api.md`.
- `docs/ai/models/fr_regressor_v3.md:29` —
  `../../adr/0041-onnx-runtime-dispatch.md` → real slug is
  `0041-lpips-sq-extractor.md`. (Both v3-ADR-pointers look like an
  unfinished rename audit.)
- `docs/rebase-notes.md:531` — `docs/adr/NNNN-slug.md` (literal
  template placeholder leaked into a tracked doc).
- `docs/rebase-notes.md:2580` — `../core/test/test_ring_buffer.c#L23`
  — file does not exist (no `test_ring_buffer.c` under `core/test/`).
- `docs/rebase-notes.md:4171` — `../core/src/cuda/ring_buffer.c`
  does not exist.
- `docs/rebase-notes.md:4724` — narrative `ADR-0049` mention; no
  ADR-0049 exists. Likely a placeholder for the (later renumbered)
  scaler-sidecar ADR.

### CHANGELOG.md (root-anchored — every `../…` path is broken)

CHANGELOG.md sits at the repo root; relative links must start with
`docs/…` / `tools/…` / `ai/…` / `scripts/…`, **not** `../…`. The
following entries leak the path style of an in-`docs/` file (almost
certainly verbatim-copied from ADR bodies during release-please runs):

- `CHANGELOG.md:3131,3132,3144,3146,3261,3303,3327,3345,3364,3371,3433,3480,3484,3491,3493,3502,3505,3511,3513,3518` —
  paths begin with `../docs/…`, `../tools/…`, `../ai/…`, `../scripts/…`.
  All resolve to `/home/kilian/dev/docs/…` (one level above the repo).
- `CHANGELOG.md:3555,3556,3557,3566,4148,4149,4299,4332` — same
  pattern with `../../docs/…` / `../../ai/…` (two levels up). Likely
  copy-pasted from a `docs/adr/_index_fragments/` body where two
  `../` were correct.
- `CHANGELOG.md:366,368,1122,2729` — bare `docs/adr/…` /
  `core/test/…` paths that resolve fine in source but point at
  files that no longer exist (e.g.
  `docs/adr/0138-simd-bit-exactness-policy.md`,
  `docs/adr/0140-ssimulacra2-simd-bitexact.md`,
  `docs/adr/0178-integer-adm-vulkan.md`,
  `core/test/test_ring_buffer.c`). Slug-drift / file-removed.

Decision the maintainer should take: either rewrite these path-prefix
errors in-place, or stop emitting `../` paths from
`docs/adr/_index_fragments/` bodies via the release-please
configuration so future entries land path-correct.

## Broken ADR references

- `docs/metrics/features.md:67` — `ADR-0268` not found on disk.
- `docs/rebase-notes.md:4724` — `ADR-0049` not found on disk.
  (See "Stale content" for the slug-drift cluster — every wrong-slug
  link above is also effectively a broken ADR reference.)

## Broken anchors

- `docs/mcp/embedded.md:165` — link target
  `../adr/0209-mcp-embedded-scaffold.md#what-lands-next-t5-2b-roadmap-per-research-0005--next-steps`.
  The heading exists but its slug, when normalised, drops the
  literal `§ "Next steps"` punctuation. Re-derive the slug after
  GitHub's renderer (likely
  `#what-lands-next-t5-2b-roadmap-per-research-0005--next-steps` —
  double dash from the §). Sanity-check by clicking the heading on
  GitHub.

## Stale content

- `tools/vmaf-tune/README.md:1,5,8,29,47` — title, scope blurb, and
  layout comment all still claim "Phase A scaffold / libx264 only".
  Real state: 17 codec adapters under
  `tools/vmaf-tune/src/vmaftune/codec_adapters/` (libaom, svtav1,
  vvenc, x264, x265, av1/h264/hevc × {nvenc, qsv, amf, videotoolbox}).
  The "Phase B/C not implemented here" comment is also stale — Phase B
  (`recommend`) and ladder/per-shot/saliency subcommands ship today.
- `docs/usage/vmaf-tune.md:10,18,21,27,35,56,92,115,226,228,381,408,
  417,436,500,550,582,605,610,675` — many "Phase A" framings, the
  "Codecs wired so far: libx264 / libx265" line at L35, and
  "extra_params (Phase A: [])" at L436. The doc itself documents
  `compare`, `tune-per-shot`, `recommend-saliency`, `ladder`, `fast`
  subcommands further down, so this is purely a stale top-of-doc
  framing problem. Recommend a single editing pass that strips the
  six-phase roadmap framing and replaces it with a current-feature
  matrix.
- `docs/state.md` — file is current (last `## Update protocol` step
  matches CLAUDE.md §12 r13). No stale rows spotted.
- `docs/rebase-notes.md` is 9 055 lines and contains both
  template leakage (line 531: `docs/adr/NNNN-slug.md`) and
  references to two source files that have been removed
  (`core/test/test_ring_buffer.c`, `core/src/cuda/ring_buffer.c`).
  These look like un-updated entries from before the
  ring-buffer refactor.

## Missing surface docs (per ADR-0100 per-surface bar)

### CLI / tools

- **Per-codec adapter pages absent.** All 17 adapters share two
  `docs/usage/vmaf-tune*.md` pages. ADR-0100 expects each
  user-discoverable surface to have its own page; here the
  hardware-accelerated adapters (`av1_amf`, `av1_nvenc`, `av1_qsv`,
  `h264_amf`, `h264_nvenc`, `h264_qsv`, `h264_videotoolbox`,
  `hevc_amf`, `hevc_nvenc`, `hevc_qsv`, `hevc_videotoolbox`) each
  have only 2–6 mentions in `docs/usage/vmaf-tune*.md`. At minimum:
  one summary table mapping `<adapter> → <minimum host requirements,
  CRF range, preset mapping, known caveats>` so users picking a
  codec on hardware they own don't need to read the source.
- **`tools/vmaf-roi-score/README.md` exists but no
  `docs/usage/vmaf-roi-score.md`** — actually this file *does*
  exist (`docs/usage/vmaf-roi-score.md`). False alarm.

### libvmaf C API headers

- `core/include/libvmaf/libvmaf_mcp.h` ships but is not mentioned
  in any `docs/api/*.md`. ADR-0100 per-surface bar requires a doc.
  Either fold a section into `docs/api/index.md` or split into
  `docs/api/mcp.md`. (Independent from `docs/mcp/`, which covers
  the *server*; this gap is on the *embedded C API*.)
- `core/include/libvmaf/feature.h`, `model.h`, `picture.h`,
  `vmaf_assert.h` lack dedicated `docs/api/*.md` pages. They are
  mentioned in `docs/api/index.md` so this is a per-surface-bar
  judgement call rather than an outright omission; flag for triage.

### Backends

- `docs/backends/x86/` only has `avx512.md`. AVX2 paths exist
  throughout `core/src/feature/x86/` (`adm_avx2.c`,
  `motion_avx2.c`, `psnr_hvs_avx2.c`, `iqa_convolve_avx2.c`,
  `ssim_avx2.c`, …) — no `docs/backends/x86/avx2.md`.
- `docs/backends/arm/overview.md` is the sole ARM/NEON page despite
  ~14 NEON kernels under `core/src/feature/arm64/`. Per-kernel
  pages aren't required, but a kernel-coverage table on
  `overview.md` would close the bar.

### Tiny-AI

- `docs/ai/models/` has 13 model cards but no
  `docs/ai/models/saliency_student_v1.md`-vs-`mobilesal.md` deduping
  note (both files exist; the user-facing relationship is unclear).
  Not strictly missing per ADR-0042's 5-point bar — call it a
  navigation gap.

## ADR index drift (TOC)

`docs/adr/README.md` is missing rows for **33 ADRs**, including
0217–0223 (cluster), 0235–0252 (cluster), and 0254–0277 (cluster).
The `_index_fragments/` directory has fragments for some but not
all of these (71 ADRs lack a fragment, 2 fragments are orphaned —
`0270-saliency-student-fork-trained-on-duts.md` and
`0287-vmaf-tune-saliency-aware.md` have no matching ADR). Per
ADR-0221's fragment pattern, this is the regeneration substrate
that's gone out of sync with the actual ADR tree. Fixing it is a
single rerun of whatever fragment-generator script the project
uses, plus reconciling the two orphans (most likely they want
matching ADRs renamed `0270-…` and `0287-…` to land).

## External links spot-checked

5 external URLs probed; 2 broken:

- `docs/usage/external-resources.md:21` —
  `https://gist.github.com/Audition-CSBlock/bef34e553132efad883c0f128c46d638`
  → 404 (gist deleted).
- `docs/usage/external-resources.md:22` —
  `https://github.com/CrypticSignal/video-quality-metrics/blob/master/CRF%2023.png`
  → 404 (file removed/renamed in upstream repo). The parent repo
  link on the same line still works.
- `https://github.com/Netflix/vmaf/blob/master/resource/doc/release.md`
  → 200 OK.
- `https://github.com/Netflix/vmaf/commit/b949cebf` → 200 OK.
- `https://docs.sigstore.dev/cosign/overview/` → 301 (redirect
  follows fine; not a problem).

External-link audit was deliberately a sample, not exhaustive. A
full sweep would need a link-checker pass on every absolute URL
under `docs/`.

---

Counts at a glance: **57 broken internal links**, **2 broken
ADR-NNNN references**, **1 broken anchor**, **2 broken external
links** in the sample, **33 ADRs missing from the index**, **71
ADRs without index fragments**, **2 orphan fragments**, multi-page
"Phase A only" stale framing in `vmaf-tune` docs, missing
per-surface docs for AVX2 / NEON kernel coverage and
`libvmaf_mcp.h`.
