<!-- markdownlint-disable MD013 -->
# Rebase-sensitive invariants

Cross-package invariants that any upstream-sync or rebase agent must preserve.
Referenced from the canonical [`AGENTS.md`](../../AGENTS.md) harness. Per-subtree
detail (the load-bearing reasons and mechanics) lives in the `AGENTS.md` under
each subtree; this page is the index. When a rebase touches a cited translation
unit, read that subtree harness before resolving conflicts. A subtree
`AGENTS.md` with an `AGENTS.d/` next to it is a generated index: read the pages
its table names for the paths you touch, and add an invariant as a page there
([agents index and topic pages](agents-index.md)).

The invariants are grouped by area. A GPU or SIMD twin that returns the CPU
extractor's bits has one entry per backend; find the feature's section, then the
backend within it.

- [Documentation and site](#documentation-and-site): entry points, the site toolchain, navigation and search
- [Build, test and CI](#build-test-and-ci): test-runner sanitisation, editor settings, coverage and CI pins, the dev container, nox, security evidence
- [Upstream sync and provenance](#upstream-sync-and-provenance): the recorded upstream head, deliberate deviations, upstream ports
- [Backends, extractors and the parity gate](#backends-extractors-and-the-parity-gate): which backends and extractors exist, how twins are registered and declared exact
- [Floating-point policy and device contracts](#floating-point-policy-and-device-contracts): contraction, libm, strict-FP lines, SYCL scratch / fp64 / sub-group rules
- [psnr_hvs](#psnr_hvs): CUDA, SYCL and HIP twins, the masking threshold
- [psnr and float_moment](#psnr-and-float_moment): integer block sums, float squares, the NEON / SVE2 order
- [SpEED and CAMBI](#speed-and-cambi): fp64 expressions, device-resident pipelines, the parity fixture
- [SSIMULACRA 2](#ssimulacra-2): CPU sum order and single-readback pipelines
- [SSIM and MS-SSIM](#ssim-and-ms-ssim): decimation, raster-order sums, option parity
- [Motion](#motion): SAD order and the five-frame window
- [VIF](#vif): statistic arithmetic, the log2 table
- [ADM](#adm): integer and float ADM, AIM, the divide, row rounding
- [CIEDE2000](#ciede2000): CPU arithmetic on each backend

## Documentation and site

- **Documentation entry points**: keep `README.md` concise and link to the
  topic guides for changing build requirements, backend coverage and model
  defaults. `docs/index.md` and `docs/backends/index.md` should link to backend
  guides rather than repeat kernel counts or maturity summaries. Keep the
  repository-root build instructions in `docs/getting-started/index.md` and
  include Meson's `core/` source directory when showing a configure command.

- **Documentation site design layer ([ADR-1508](../adr/1508-docs-site-toolchain-and-charts.md))**:
  the design is `docs/stylesheets/vmafx.css` and nothing else: no template
  override, no Material colour name and no `font:` block in `mkdocs.yml`, so
  Zensical's `classic` variant can render it later. The fonts in
  `docs/assets/fonts/` match their `vendor.json`
  (`scripts/docs/check_vendored_assets.py`, run by
  `make docs-fragments-check`). The landing page keeps its `vx-*` wrappers when
  its text changes. See [Documentation site design](docs-site-design.md).

- **Documentation charts ([ADR-1508](../adr/1508-docs-site-toolchain-and-charts.md))**:
  `scripts/docs/generate-charts.py` writes each chart's data, both SVG renders,
  the page block between its `CHART` sentinels and the vendored Vega bundle;
  `make docs-fragments-check` compares them with a fresh build and the docs CI
  jobs re-render with `--require-render`. Regenerate after a change to
  `scripts/ci/exact_twins.d/`, `LIBM_TWINS`, `scripts/ci/upstream_parity.d/`
  or the 576x324 snapshots; keep the sentinels when a page is rewritten. See
  [Documentation site design](docs-site-design.md#charts).

- **Documentation diagrams ([ADR-1508](../adr/1508-docs-site-toolchain-and-charts.md))**:
  diagrams are figure specs in `docs/figures/` rendered into
  `docs/assets/figures/` by `tools/figures/`, shown through the MkDocs hook
  listed in `mkdocs.yml`; `make docs-figures` holds the renders to their specs
  and every evidence anchor to the code. No Mermaid fence and no ASCII diagram
  returns on a page that has a figure. See
  [Documentation site design](docs-site-design.md#diagrams).

- **ADR navigation collapsed ([ADR-1510](../adr/1510-adr-nav-collapse-behind-index.md))**:
  the `ADRs` entry of `mkdocs.yml` lists the ADR index, the template and the
  tag index only; no `ADR-NAV-GENERATED` block and no
  `scripts/docs/generate-adr-nav.sh`. `test_adr_navigation_is_collapsed` in
  `scripts/docs/tests/test_generators.py` guards it. See
  [scripts/AGENTS.md](../../scripts/AGENTS.md).

- **Site search covers user pages only ([ADR-1512](../adr/1512-docs-search-user-pages-only.md))**:
  `.meta.yml` files under `docs/adr/`, `docs/research/` and
  `docs/changelog-archive/` set `search.exclude` through the `material/meta`
  plugin; the index pages and the generated title lists
  (`scripts/docs/generate-record-titles.py`) override it, and
  `docs/rebase-notes.md` and `docs/state.md` keep their front matter.
  `scripts/docs/check_search_scope.py` reads the built index. See
  [Documentation site design](docs-site-design.md#search).

## Build, test and CI

- **Root licence files ([ADR-1699](../adr/1699-root-licence-files-eupl.md))**:
  the root holds `LICENSE` (the EUPL-1.2, byte for byte `LICENSES/EUPL-1.2.txt`)
  and `NOTICE` (Netflix's `LICENSE`, unchanged). An
  upstream sync that changes Netflix's `LICENSE` applies it to
  `NOTICE`; no sync brings back `LICENSE-MIT` or another
  root licence file. Package licence fields name the licences of the files the
  package ships, and fork `.toml` files carry an EUPL-1.2 header. The root
  `go.mod` keeps its `retract [v1.0.0-rc.1, v1.0.0-rc.2]`.
  `scripts/ci/check_licence_metadata.py` (required
  `Licence Provenance` job and a pre-commit hook) refuses each breach. See
  [scripts/ci/AGENTS.md](../../scripts/ci/AGENTS.md).

- **Meson test secret environment sanitization ([ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md))**:
  `scripts/ci/run_meson_test.py` deletes sensitive GitHub credential keys before Meson starts
  and records its raw parent environment in `testlog.txt`. Every supported Make, workflow,
  preflight, bisection, setup-guidance, and Zed entry point must remain on that wrapper.
  `core/meson.build` retains a default test setup using `environment().unset()` for
  (`GITHUB_PERSONAL_ACCESS_TOKEN`,
  `GITHUB_TOKEN`, `GH_TOKEN`, `GH_ENTERPRISE_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `GITHUB_PAT`,
  `GH_PAT`, `GITHUB_AUTH_TOKEN`, `GITHUB_API_TOKEN`, `HOMEBREW_GITHUB_API_TOKEN`,
  `ACTIONS_ID_TOKEN_REQUEST_TOKEN`, `ACTIONS_RUNTIME_TOKEN`) at the child and JSON-log layer.
  The regression contract rejects raw supported-entry-point bypasses, alternate setups, and
  explicit forbidden-name reintroduction. Preserve the runner, callers, setup, and
  `core/test/test_meson_secret_env_sanitization.py` together. Raw external Meson/Ninja test
  commands are outside this bounded guarantee.

- **Zed project settings are project-scoped**: `.zed/settings.json` is parsed
  as Zed's `ProjectSettingsContent`, so it must not regain `agent`,
  `agent_servers`, provider/model pins, or permission policy. Preserve the
  current `docker exec -i vmaf-dev-mcp vmafx-mcp` context-server entry, the
  three `Standards:` tasks, and the contract in
  `scripts/ci/tests/test_zed_project_config.py`. The scoped mechanics live in
  [`.zed/AGENTS.md`](../../.zed/AGENTS.md).

- **VMAFx API compat shims own four libvmaf entry points ([ADR-1852](../adr/1852-vmafx-api-redesign.md))**:
  `vmaf_init`, `vmaf_close`, `vmaf_version` and `vmaf_feature_score_at_index`
  are generated from `core/api/vmafx.toml` into
  `core/src/vmafx/compat_libvmaf_gen.c` on the `vmafx_*` API; their former
  bodies are `vmaf_engine_*` in `core/src/libvmaf.c`. An upstream sync that
  changes one of the four ports the change into its `vmaf_engine_*` body and
  never re-adds the old definition (duplicate symbol). Generated files are
  never hand-merged: take either side and run
  `python3 scripts/codegen/vmafx-api.py --write`; the Meson test
  `test_vmafx_api_generated_current` fails on any difference. `libvmaf`
  links the generated version script `core/src/vmafx.map` on ELF targets
  (`-Wl,--no-undefined-version`), and `check_exported_symbols` compares the
  `vmafx_` exports with `core/src/vmafx_symbols.txt`; keep both when a sync
  touches the library target. See [core/src/AGENTS.md](../../core/src/AGENTS.md).
- **VMAFx imported frames never take a host copy ([ADR-1929](../adr/1929-vmafx-device-frames-fences.md))**:
  `vmafx_frame_import()` binds the producer's planes or converts NV12 /
  P010 / P016 on the device (de-interleave, P010 shift 6) and nothing else;
  every place that copies imported pixels through host memory calls
  `vmafx_count_host_copy()` (`core/src/vmafx/frame_import_hooks.h`), and the
  import tests assert the count stays 0. A frame's release fence is signalled
  where its last picture reference is dropped (`vmafx_frame_release()`,
  `pool_frame_release()`), never when a submit returns. The CPU conversion
  uses the row readers of `core/src/metal/iosurface_layout.h`: a sync that
  changes them keeps `test_vmafx_import_bitexact` and
  `test_metal_iosurface_layout` passing together.
- **VMAFx CUDA frames are read on one stream per device ([ADR-2023](../adr/2023-vmafx-cuda-device-frames.md))**:
  every frame of a CUDA device of the VMAFx API is a CUDA picture on the
  device's library stream; its acquire wait, conversions and ready event are
  there, and its release is enqueued there where its last reference is
  dropped (`core/src/cuda/import_fence.c`). `core/src/libvmaf.c` skips the
  ADR-1199 barrier only for a pair of `ordered` pictures and never downloads a
  `vmafx` picture. `integer_vif_cuda` reads each picture with its own pitch
  (`VifBufferCuda.dis_stride`). Preserve `test_vmafx_import_cuda*` together.
- **VMAFx HIP frames are read on one stream per device ([ADR-2092](../adr/2092-vmafx-hip-device-frames.md))**:
  every frame a HIP device of the VMAFx API imports is a
  `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE` picture on the device's library
  stream. The twins copy it there (`vmaf_hip_picture_upload()`, the shared
  frame), and the reading twin's stream and the null stream wait for the
  copies; `bind_hip_frame()` submits each import with `hipStreamQuery()`; a GL
  import exports GL textures as dma-bufs (`core/src/vmafx/egl_export.c`, [ADR-2132](../adr/2132-hip-gl-textures-through-egl-dmabuf.md); the runtime's HIP-GL interop is never called);
  a dma-buf is imported with its own size. Keep these when rebasing
  `core/src/hip/picture_hip.c`, `shared_frame.c` or the three twins that
  stage on the host. `core/test/test_vmafx_import_hip_contract.py`,
  `test_hip_shared_frame` and `test_vmafx_import_hip*` (on a device) guard
  them together.
- **Coverage Gate ratchet + per-PR delta gate (ADR-0922)**:
  [ADR-0922](../adr/0922-coverage-ratchet-aggressive.md). Absolute
  floors live in `scripts/ci/coverage-check.sh`
  (`OVERALL_MIN=70`, `CRITICAL_MIN=90`, `PER_FILE_MIN[...]`); per-PR
  drop tolerance lives in `scripts/ci/coverage-delta-check.sh`
  (default 0.5pp on overall and per-touched-file). Lowering any
  floor or loosening the delta tolerance requires a new ADR
  superseding ADR-0922. The Coverage Gate job in
  `.github/workflows/tests-and-quality-gates.yml` invokes both
  scripts; the delta gate needs `actions/checkout` with
  `fetch-depth: 0` because it runs `git merge-base`. See
  [scripts/ci/AGENTS.md](../../scripts/ci/AGENTS.md) §Coverage Gate
  ratchet for the full coupling.

- **CI action pins — Windows MSVC dev env**
  ([ADR-0635](../adr/0635-ci-warning-omnibus-2026-05-19.md)):
  `.github/workflows/libvmaf-build-matrix.yml` uses
  `TheMrMilchmann/setup-msvc-dev@79dac248…` (v4.0.0, Node.js 24) for the
  Windows GPU build legs. If upstream ADR-0121 is re-implemented or the
  Windows legs are rebased, do **not** reintroduce `ilammy/msvc-dev-cmd`
  (Node.js 20, deprecated 2026-06-02). The `TheMrMilchmann` action is a
  drop-in replacement with identical `vcvarsall.bat` semantics.
  Also: both Windows jobs are pinned to `windows-2025`; do not revert to
  `windows-latest` (redirect to `windows-2025-vs2026` takes effect
  2026-06-15).

- **dev-MCP Docker container**
  ([ADR-0451](../adr/0451-local-dev-mcp-container.md)):
  `dev/Containerfile` installs CUDA through the shared installer's exact
  `--mode=full` contract (ADR-1306). `build-config.env` owns the apt series,
  release lock, and exact toolkit/nvcc/cudart package versions; do not restore a
  floating `cuda-toolkit-13-4` command in the Containerfile. It also pins the unversioned
  `intel-basekit` meta-package (Intel does not publish a
  `intel-basekit-2025.3` apt package), and the digest-pinned
  `rocm/dev-ubuntu-26.04:10.0.0-full` image in the `rocm-src` stage
  (ADR-1225 / ADR-1231). If SDK versions are bumped (routine security
  maintenance), update their shared pins in `build-config.env` and regenerate
  the mirrors before merging; a ROCm bump
  additionally means re-validating the `rocm-src` prune list against its
  hipcc smoke check.
  `dev/scripts/smoke-probe-loop.sh` assumes the golden pair lives at
  `${VMAF_TESTDATA_PATH}/ref_576x324_48f.yuv` / `dis_576x324_48f.yuv`
  — do not rename these files. The probe JSON schema fields (`ts`,
  `host_id`, `backend_results`, `mcp_results`) are an internal format;
  update `docs/development/dev-mcp.md` if the schema changes. This
  directory does not affect the libvmaf C build or any CI gate.

- **Top-level `noxfile.py` is a local-dev affordance, not a CI gate (ADR-0914)**:
  The repo-root `noxfile.py` exposes one session per Python suite
  (`ai`, `compat_decorator`, `mcp`, `vmaf_tune`, `dev_llm`,
  `roi_score`, `ensemble_kit`, `rc1_tester`, `tooling`, `python_harness`)
  plus `all` / `lint` meta-sessions. CI does **not** call nox: each suite
  runs in its own job (`Tiny AI`, `MCP Smoke`, `RC1 Tester Report`,
  `Tooling Tests`, one `Python Package Tests` leg per remaining package),
  which installs the suite's hash lock and runs `pytest -rs`.
  [`.github/test-suites.json`](../../.github/test-suites.json) maps every
  tracked test file to one suite and every suite to its required checks;
  `scripts/ci/suite_registry.py check` fails on an unwired test file
  ([ADR-1528](../adr/1528-test-suite-registry.md)). When adding a new
  Python package, update `noxfile.py`, the CI job and the registry
  together. See [test suites](test-suites.md) and
  [`docs/development/python-test-orchestrator.md`](python-test-orchestrator.md).
  The `python_harness` session intentionally delegates to `tox -c
  python` rather than duplicating the Cython + Netflix golden-data
  setup that lives in `python/tox.ini`; do not collapse them.

- **Security support and badge evidence** — `SECURITY.md` describes actual
  VMAFx release support, not inherited Netflix/libvmaf version strings.
  Keep [the passing worksheet](best-practices-assessment.md)
  tied to a reviewed source revision and the live project record. Configuration,
  future releases and agent-authored prose cannot establish historical response
  times, a human developer's knowledge or a completed external badge. The
  project website is GitHub Pages; keep the short purpose and participation
  links in `docs/index.md`, and verify deployed pages before citing new text.

## Upstream sync and provenance

- **Recorded upstream head ([ADR-1474](../adr/1474-relicense-helper-headers-and-ci-check.md))**:
  `docs/development/known-upstream-bugs.md` carries exactly one heading
  ``## Upstream head the fork is at parity with: `<commit id>` (<date>)``.
  `scripts/ci/upstream_parity_pin.py` reads it and the required check
  `Licence Provenance` compares every file's licence header against that
  Netflix/vmaf commit. An upstream port or sync moves the id in the same pull
  request and keeps the heading's wording; a second heading of that form, or a
  reworded one, fails the check. A port that brings a file whose path or name
  now exists upstream changes that file's verdict: run
  `scripts/dev/relicense_fork_files.py --check --upstream-ref <new id>`
  before pushing. See [the guide](licence-provenance-check.md).

- **Deliberate deviations from Netflix's source, by ADR**: code inherited
  from Netflix/vmaf evaluates as Netflix's source does unless an ADR says
  otherwise. Eight fixes that predate that rule have their ADR since
  2026-10-02, each with upstream's lines at Netflix `9e48141b`, the measured
  size and the upstream pull request that would end it:
  [ADR-1479](../adr/1479-ciede-422-chroma-subsampling-flags.md) (`ciede`
  4:2:2 chroma flags),
  [ADR-1480](../adr/1480-speed-frame-buffers-prescale-above-one.md)
  (`speed_temporal` buffers at `speed_prescale` above 1),
  [ADR-1481](../adr/1481-extractor-failure-fails-the-run.md) (a worker's
  error fails the run),
  [ADR-1482](../adr/1482-integer-adm-frames-17-to-32.md) (integer `adm` on
  frames of 17 to 32 pixels),
  [ADR-1483](../adr/1483-odd-size-chroma-planes-round-up.md) (odd-sized
  chroma planes round up),
  [ADR-1484](../adr/1484-float-ms-ssim-magnitude-before-pow.md)
  (`float_ms_ssim` magnitude before `pow()`),
  [ADR-1485](../adr/1485-apsnr-zero-error-plane-reports-cap.md) (`apsnr` of a
  plane without error) and
  [ADR-1486](../adr/1486-float-motion-scale1-uses-callers-stride.md)
  (`float_motion` scale-1 stride). A sync keeps the fork's side of these
  lines until the named upstream pull request is merged; the table is in
  [rebase-notes](../rebase-notes.md) under "Eight deliberate deviations".

- **Upstream port — feature/motion options from b949cebf
  (T-NEW-1)**: PR #197 (`b949cebf`, MERGED 2026-04-29) ported
  Netflix's feature/motion several-options commit; PR #213 ported
  `d3647c73` `feature/speed` extractors (`speed_chroma` +
  `speed_temporal`; `speed.c` is in the tree).

## Backends, extractors and the parity gate

- **GPU long-tail terminus reached** — every registered feature
  extractor has at least one GPU twin (lpips remains ORT-delegated
  per [ADR-0022](../adr/0022-inference-runtime-onnx.md)).
  Cross-backend tolerances live in
  `scripts/ci/cross_backend_parity_gate.py`. Governing ADRs:
  [ADR-0182](../adr/0182-gpu-long-tail-batch-1.md) (batch 1: psnr /
  ciede / moment), [ADR-0188](../adr/0188-gpu-long-tail-batch-2.md)
  (batch 2: ssim / ms_ssim / psnr_hvs),
  [ADR-0192](../adr/0192-gpu-long-tail-batch-3.md) (batch 3:
  motion_v2 / float-twins / ssimulacra2 / cambi; `float_ansnr` removed
  in commit 70ed8b3ce3 / PR #38).
  See [core/src/feature/AGENTS.md](../../core/src/feature/AGENTS.md).

- **Vulkan backend removed ([ADR-0726](../adr/0726-drop-vulkan-backend.md))** —
  the Vulkan backend, its `libvmaf_vulkan.h` surface, the `core/src/vulkan/`
  tree, the Volk-symbol-hiding machinery, and all `*_vulkan` GLSL kernels
  (ssim / ms_ssim / motion_v2 / cambi / psnr chroma) no longer exist in the
  tree. No rebase invariant survives. Treat any lingering Vulkan reference as
  stale.

- **MCP embedded server (ADR-0128, ADR-0209; runtime v3)**:
  [ADR-0209](../adr/0209-mcp-embedded-scaffold.md). Public header
  `libvmaf_mcp.h`; the runtime is live in `core/src/mcp/` (`mcp.c`,
  `dispatcher.c`, `compute_vmaf.c`, the `transport_{stdio,uds,sse}.c` bodies,
  vendored cJSON) behind `enable_mcp` plus three transport sub-flags. The
  stdio transport is newline-delimited JSON-RPC; `-ENOSYS` means only "feature
  or transport not built"; the SPSC command ring is v4 work, so
  `queue_depth` / `max_drain_per_frame` are validated and stored, nothing
  more. User page: [embedded MCP](../mcp/embedded.md). See
  [core/src/mcp/AGENTS.md](../../core/src/mcp/AGENTS.md).

- **HIP backend (T7-10, [ADR-0212](../adr/0212-hip-backend-scaffold.md), PR #200)** —
  public `libvmaf_hip.h`, 19 registered feature extractors
  ([HIP overview](../backends/hip/overview.md)), `enable_hip` meson option
  default `false`, device kernels behind `enable_hipcc`.

- **SVE2 SIMD ports (T7-38, [ADR-0213](../adr/0213-ssimulacra2-sve2.md), PR #201)** —
  SSIMULACRA 2 PTLR + IIR-blur SVE2 ports developed against
  `qemu-aarch64-static`. Same bit-exact contract as the existing
  NEON ports.

- **GPU-parity CI gate (T6-8, ADR-0214)**:
  [ADR-0214](../adr/0214-gpu-parity-ci-gate.md). Single source of
  truth for cross-backend tolerances:
  `scripts/ci/cross_backend_parity_gate.py`. Adding a new GPU twin
  requires (1) `FEATURE_METRICS` entry, (2) `FEATURE_TOLERANCE` entry
  if it relaxes places=4, (3) row in
  `docs/development/cross-backend-gate.md`. Declaring a twin
  bit-identical adds one file `scripts/ci/exact_twins.d/<feature>.<backend>`
  ([ADR-1428](../adr/1428-exact-twins-fragments.md)) and edits no shared
  line; on a conflict in the generated
  `docs/development/cross-backend-exact-twins.md` take master's side and run
  `make docs-fragments-write`. See
  [core/AGENTS.md](../../core/AGENTS.md).

- **FastDVDnet temporal pre-filter (T6-7, [ADR-0215](../adr/0215-fastdvdnet-pre-filter.md),
  PR #203)** — 5-frame window pre-filter feeding ssim/ms_ssim.

- **MobileSal saliency extractor (T6-2a, [ADR-0218](../adr/0218-mobilesal-saliency-extractor.md),
  PR #208)** — first half of T6-2 (encoder-side ROI bundle).
  Saliency-weighted VMAF, sidecar emit for `tools/vmaf-roi`.

- **TransNet V2 shot-boundary extractor (T6-3a, PR #210)** —
  ~1M params; feeds `tools/vmaf-perShot` CRF predictor.

- **Model registry + Sigstore (T6-9, [ADR-0211](../adr/0211-model-registry-sigstore.md), PR #199)**:
  `--tiny-model-verify` flag + registry schema + Sigstore bundle
  paths. Pairs with
  [ADR-0010](../adr/0010-sigstore-keyless-signing.md) (release
  signing).

- **CPU extractors declare the features they write ([ADR-1359](../adr/1359-cli-feature-backend-twin.md))**:
  the twin lookup pairs a CPU extractor with a device twin through
  `provided_features`. `core/src/feature/float_moment.c` is an upstream-mirror
  file whose list the fork changed from upstream's pseudo-name
  `"float_moment"` to the four emitted `float_moment_*` names; an upstream
  sync must keep the fork's list, or `--backend <gpu> --feature float_moment`
  falls back to the CPU again. `vmaf_feature_extractor_twin_audit()` and
  `test_every_device_twin_is_reachable` (`core/test/test_feature_extractor.c`)
  fail when any registered device twin is unreachable. See
  [core/src/feature/AGENTS.md](../../core/src/feature/AGENTS.md).

- **CUDA twins declared exact as a group ([ADR-1457](../adr/1457-cuda-exact-twins-declared.md))**:
  `scripts/ci/exact_twins.d/{motion,motion_debug,motion_v2,psnr,float_ssim,float_ssim_lcs,float_ms_ssim,float_ms_ssim_lcs,cambi}.cuda`
  make the parity gate compare those cells with tolerance 0, and
  `core/test/test_cuda_exact_twins.c` holds `motion_cuda`, `motion_v2_cuda`,
  `psnr_cuda`, `float_ssim_cuda`, `float_ms_ssim_cuda` and `cambi_cuda` to
  `==` on every output. A rebase that changes one of these twins or its CPU
  extractor keeps them bit-identical; a twin that drifts is fixed, never
  given a tolerance or taken off the list.

- **SYCL twins declared exact as a group ([ADR-1451](../adr/1451-sycl-exact-twins-declared.md))**:
  `scripts/ci/exact_twins.d/{adm,motion,motion_debug,motion_v2,psnr,float_ssim,float_ssim_lcs,cambi}.sycl`
  make the parity gate compare those cells with tolerance 0, and
  `core/test/test_sycl_exact_twins.c` holds `adm_sycl`, `motion_sycl`,
  `motion_v2_sycl`, `psnr_sycl`, `float_ssim_sycl` and `cambi_sycl` to `==`
  on every output. A rebase that changes one of these twins or its CPU
  extractor keeps them bit-identical; a twin that drifts is fixed, never
  given a tolerance or taken off the list.

## Floating-point policy and device contracts

- **No C or C++ translation unit is built with FP contraction ([ADR-1461](../adr/1461-strict-fp-every-translation-unit.md))**:
  `core/src/meson.build` declares `vmaf_strict_fp_args` as a project argument
  for C and C++ directly after the `VMAF strict FP compiler-argument policy`
  block, above the first build target. Keep both there on a rebase (Meson
  refuses `add_project_arguments()` after a target), and never give a target
  `vmaf_fp_model_args` alone or any flag that turns contraction back on.
  `core/test/test_strict_fp_compiler_args.py` reads the compile database of
  the build it runs in; `make test-netflix-golden-arm64` runs the golden gate
  on an aarch64 cross build, where a clang build and a GCC build used to
  differ. See [core/AGENTS.md](../../core/AGENTS.md).

- **icx and icpx builds link glibc's libm, not Intel's libimf ([ADR-1495](../adr/1495-icx-system-libm.md))**:
  `core/src/meson.build` declares the `VMAF host math library link policy`
  block directly after the strict FP policy and passes its lists with
  `add_project_link_arguments()` for C and C++, above the first build target:
  an `intel-llvm` compiler gets `-no-intel-lib=libimf`, every other compiler
  nothing. The Intel driver otherwise links `libimf` into every link (it turns
  a given `-lm` into `-limf -lm`), and an icx-built `vmaf` exported libimf's
  copies of the math functions `libvmaf.so` imports, so the CPU scores of an
  icx build differed from a GCC build's. Keep the block and both lines on a
  rebase, and never link Intel's math library back by name or substitute
  `-shared-intel`. `core/test/test_icx_system_libm.py` reads the build's own
  `libvmaf.so` and `vmaf` (skips on non-icx builds) and
  `core/test/test_strict_fp_compiler_args.py` executes the block per compiler
  pair. See [core/AGENTS.md](../../core/AGENTS.md).

- **GPU device code is stored compressed ([ADR-1590](../adr/1590-device-code-compression.md))**:
  `core/src/meson.build` defines one compression list per backend between the
  `BEGIN/END VMAF {CUDA,HIP,SYCL} device code compression policy` markers
  (`cuda_compress_args`, `hip_compress_args`, `sycl_compress_args`), gated by
  the `compress_device_code` option (default `true`). Every nvcc fatbin, every
  `hipcc --genco` command (the test probes in `core/test/meson.build` too),
  the SYCL AOT compile, `sycl_link_args` and the MSVC device link take the
  list; the flags are spelled nowhere else. A rebase that adds a device compile
  site adds the list, and must not drop the `error()` that refuses a compiler
  without compression. The build checks its own output with
  `core/src/check_device_compression.py`; `core/test/test_device_code_compression.py`
  guards the policy without a device. See
  [core/AGENTS.d/device-code-compression.md](../../core/AGENTS.d/device-code-compression.md).
- **SYCL strict FP line on every feature TU ([ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md))**:
  `core/src/meson.build` defines `sycl_strict_fp_args` once, between the
  `BEGIN/END VMAF SYCL strict FP policy` markers: icpx gets
  `-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div
  -foffload-fp32-prec-sqrt` in that order (precise implies contraction on, so
  contraction-off must follow it), AdaptiveCpp `-ffp-contract=off`. Every
  feature TU takes it through `sycl_feature_tail_args`; no TU gets a private
  FP list. `sycl_link_args` also carries `sycl_fp32_prec_args` to every link
  the icpx driver runs, because the SPIR-V JIT image is generated there;
  dropping it leaves `-Dsycl_icpx_aot_targets=` builds with approximate `/`
  and sqrt. The MSVC build's explicit device link (ADR-1364) generates every
  image and takes `sycl_strict_fp_args` whole.
  `core/test/test_strict_fp_compiler_args.py` executes the policy and
  `test_sycl_fp_arith_contract` checks the device arithmetic.

- **CUDA device FP policy ([ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md))**:
  every CUDA fatbin takes `cuda_device_strict_fp_args` (`--fmad=false` under
  nvcc, `-ffp-contract=off` under clang CUDA), defined once between the
  `VMAF CUDA device strict FP policy` markers in `core/src/meson.build`;
  `cuda_cu_extra_flags` carries no floating-point flag. A kernel whose
  reference fuses writes `__fmaf_rn()`. `float_ms_ssim_cuda` reproduces
  `ms_ssim_decimate.c`, `iqa_convolve()` and
  `ssim_accumulate_default_scalar()` operation for operation and is
  bit-identical to the CPU. `core/test/test_strict_fp_compiler_args.py`,
  `core/test/test_cuda_kernel_source_contract.py` and
  `core/test/test_cuda_float_ms_ssim_parity.c` guard it. See
  [core/src/cuda/AGENTS.md](../../core/src/cuda/AGENTS.md) and
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **SYCL kernels use no scratch memory ([ADR-1395](../adr/1395-sycl-kernels-no-scratch.md))**:
  on an Arc A-series GPU under the Linux xe driver, kernels with a private array
  in memory or spilled registers return wrong values. `test_sycl_kernel_scratch`
  fails on a scratch kernel missing from `core/src/sycl/scratch_ratchet.txt`,
  whose extractors must match `kScratchExtractors` in
  `core/src/sycl/scratch_check.cpp`; the list only shrinks. `integer_vif_sycl` runs
  at SIMD-16 only ([ADR-1830](../adr/1830-sycl-vif-simd16-only.md)): a sync
  must not bring back its SIMD-32 kernels or `VMAF_SYCL_VIF_SUBGROUP_SIZE`,
  which spilled on Xe-LP. See
  [core/src/sycl/AGENTS.md](../../core/src/sycl/AGENTS.md) and
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **`libvmaf_sycl` imports each input with its own VA display and never skips a frame ([ADR-1761](../adr/1761-sycl-filter-import-retry-then-fail.md))**:
  FFmpeg patch `0005` reads the VA display of both inputs' QSV sessions and imports each
  input's surfaces with its own; a failed import is retried in a bounded loop and then stops
  the filter naming the frame, with no pooled score after it. Patch `0013`'s `libvmaf_metal`
  prints no pooled score after a stop either. A refresh or an upstream rebase must not
  bring back the single display, the pass-through on a failed import, or a score after a
  stop. `core/test/test_sycl_filter_import_contract.py` and
  `core/test/test_metal_iosurface_filter_contract.py` guard it without a device,
  `ffmpeg-patches/test/check-sycl-import-retry.sh` on one.
- **SYCL zero-copy admission ([ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md))**:
  `vmaf_read_pictures_sycl()` in `core/src/libvmaf.c` refuses, before counting a frame,
  every registered extractor whose `reads_shared_luma_only()` hook is absent or false for
  its options, with an error naming it and `-ENOTSUP`; `vmaf_flush_sycl()` skips
  uninitialized extractors. The hooks sit on eight SYCL twins
  (`core/src/feature/sycl/AGENTS.d/zero-copy-admission.md`); a twin that starts reading a
  host picture narrows its hook in the same PR. `test_sycl_zero_copy_admission` and
  `test_sycl_zero_copy_model_gate` guard it. See
  [core/src/sycl/AGENTS.md](../../core/src/sycl/AGENTS.md).
- **SYCL fp64-less device contract (T7-17, ADR-0220)**:
  [ADR-0220](../adr/0220-sycl-fp64-fallback.md). SYCL feature
  kernels are unconditionally fp64-free; a single fp64 instruction
  in any lambda blocks the whole TU on Arc A-series. See
  [core/src/sycl/AGENTS.md](../../core/src/sycl/AGENTS.md).

- **SYCL kernels require sub-group size 16 or 32 ([ADR-1468](../adr/1468-sycl-sub-group-sizes-every-aot-target.md))**:
  the default build compiles every kernel ahead of time for the 19 targets
  of `sycl_icpx_aot_targets`, and the Xe2 targets do not compile a kernel
  that requires 8. `core/src/feature/sycl/sycl_compat.h` rejects another
  size at compile time (`VmafSyclSubGroupSize`); a rebase must not bring a
  raw `[[sycl::reqd_sub_group_size(N)]]` or `sub_group_size<N>` into a
  kernel, nor a size 8. `core/test/test_sycl_sub_group_size_contract.py`
  (device-free) and `core/test/test_sycl_aot_default_targets.py` (suite
  `sycl-aot`, compiles every SYCL translation unit for the full default
  list) guard it; `core/test/sycl_aot_targets.py` holds the measured sizes
  per target family and needs an entry for a target added to the list.

## psnr_hvs

- **`psnr_hvs_cuda` returns the CPU's scores bit for bit ([ADR-1397](../adr/1397-psnr-hvs-twins-cpu-float-sum.md))**:
  `psnr_hvs_score.cu` stores the 64 terms `calc_psnrhvs()` sums per block, in
  the CPU's arithmetic (double masking table, the threshold's float product
  and double root, integer coefficient difference, fatbin built with
  `--fmad=false`), and
  `core/src/feature/psnr_hvs_score.c` adds them into one running `float` in the
  CPU's order. A change to `calc_psnrhvs()` or `extract()` in
  `third_party/xiph/psnr_hvs.c` changes the kernel and that file in the same
  PR. `core/test/test_psnr_hvs_twin_exact_sum_contract.py` and
  `test_psnr_hvs_score` guard it without a device, `test_cuda_psnr_hvs_parity`
  on one; the parity gate compares the twin with tolerance 0 (`EXACT_TWINS`).
  See [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **The `psnr_hvs` masking threshold is upstream's statement ([ADR-1488](../adr/1488-psnr-hvs-upstream-mask-product.md))**:
  `calc_psnrhvs()` writes `s_mask = sqrt(s_mask * s_gvar) / 32.f` (and the
  same for `d_mask`), as Netflix `libvmaf/src/feature/third_party/xiph/psnr_hvs.c:316-317`
  does: a float product, its root in double, the result stored as float. A
  sync takes upstream's side of these two lines, and no `(double)` goes in
  front of the product (PR #552 added one). `x86/psnr_hvs_avx2.c` and
  `arm64/psnr_hvs_neon.c` write the same statement in `compute_masks()`; the
  CUDA and HIP kernels form the float product and take the double root, the
  SYCL kernel takes `sqrt_rn()` of the float product (the same value without
  fp64). A change to the statement changes all six in the same PR.
  `test_psnr_hvs_dispatch_invariance` (recorded blocks scored as Netflix
  master scores them), `test_psnr_hvs_simd` and
  `test_psnr_hvs_twin_exact_sum_contract.py` guard it.

- **`psnr_hvs_sycl` and `psnr_hvs_hip` return the CPU's scores bit for bit ([ADR-1401](../adr/1401-psnr-hvs-sycl-hip-exact-twins.md))**:
  both store the 64 terms `calc_psnrhvs()` sums per block and call
  `core/src/feature/psnr_hvs_score.c`, as the CUDA twin does. The HIP kernel
  (`psnr_hvs_score.hip`) takes the masking table in `double`, the threshold
  as the float product's double root (ADR-1488), and is built with
  `-ffp-contract=off -fhip-fp32-correctly-rounded-divide-sqrt`
  (`hip_cu_extra_flags`). The SYCL kernel has no fp64: its masking table is a
  compile-time constant and its threshold comes from `sqrt_rn()` in
  `core/src/feature/sycl/sycl_exact_fp.h` applied to the float product (the
  correctly rounded fp32 root, which is the double root rounded to float); it
  must stay free of scratch memory. A change to `calc_psnrhvs()` or
  `extract()` in `third_party/xiph/psnr_hvs.c` changes both kernels in the
  same PR. `test_psnr_hvs_twin_exact_sum_contract.py` guards all three twins
  without a device; `test_sycl_psnr_hvs_parity`, `test_hip_psnr_hvs_parity`
  and `test_sycl_fp_arith_contract` on one. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md) and
  [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

## psnr and float_moment

- **psnr chroma GPU twins (T3-15(b), PR #204)** — `psnr_cb` /
  `psnr_cr` device kernels alongside the existing `psnr_y` from
  [ADR-0182](../adr/0182-gpu-long-tail-batch-1.md). (The original
  Vulkan implementation was removed with the backend in ADR-0726.)

- **`float_psnr_cuda` adds integers ([ADR-1455](../adr/1455-cuda-float-psnr-exact-block-sums.md))**:
  `core/src/feature/cuda/float_psnr/float_psnr_score.cu` forms the CPU's term
  (`diff * diff` in `float`, as `float_psnr.c` does) with `__fmul_rn()` as an
  integer in units of 1 / scaler^2 and reduces `uint64` values per warp and
  per block; `float_psnr_cuda.c::float_psnr_noise()` adds the blocks in
  `uint64` and divides the exact total by scaler^2 and the pixel count. An
  fp32 block sum is exact only up to 24 bits. A change to how `float_psnr.c`
  forms or adds its terms changes the kernel in the same PR.
  `core/test/test_cuda_float_psnr_exact_contract.py` guards it without a
  device, `test_cuda_float_psnr_parity` (`==`) on one.
  Each block / work-group lies in ONE row (256 x 1), and the host adds each
  row's exact sum into a double in row order with
  `core/src/feature/float_psnr_rows.h` ([ADR-1499](../adr/1499-float-psnr-twins-cpu-row-order.md)),
  as `float_psnr.c` adds its rows, so the twin rounds where the CPU rounds
  past 2^53 units; a sync must not bring back 16x16 blocks or a frame total
  rounded once. The HIP twin (ADR-1440) follows the same layout and helper.

- **`float_psnr_sycl` adds integers ([ADR-1450](../adr/1450-sycl-float-psnr-exact-block-sums.md))**:
  `core/src/feature/sycl/float_psnr_sycl.cpp` forms the CPU's term
  (`diff * diff` in `float`, as `float_psnr.c` does) as an integer in units of
  1 / scaler^2 and reduces `uint64` values per sub-group, per work-group and on
  the host; an fp32 group sum is exact only up to 24 bits. The host divides
  the exact total by scaler^2 and the pixel count. A change to how
  `float_psnr.c` forms or adds its terms changes the kernel in the same PR.
  `core/test/test_sycl_float_psnr_exact_contract.py` guards it without a
  device, `test_sycl_float_psnr_parity` (`==`) on one.
  Each block / work-group lies in ONE row (256 x 1), and the host adds each
  row's exact sum into a double in row order with
  `core/src/feature/float_psnr_rows.h` ([ADR-1499](../adr/1499-float-psnr-twins-cpu-row-order.md)),
  as `float_psnr.c` adds its rows, so the twin rounds where the CPU rounds
  past 2^53 units; a sync must not bring back 16x16 blocks or a frame total
  rounded once. The HIP twin (ADR-1440) follows the same layout and helper.

- **`float_moment_hip` adds the CPU's float squares ([ADR-1447](../adr/1447-hip-float-moment-cpu-float-squares.md))**:
  the 16-bit kernel of `core/src/feature/hip/float_moment/moment_score.hip`
  adds `moment_float_square()`, one fp32 product of the sample with itself
  converted to an integer, where `moment.c::compute_2nd_moment()` forms the
  square in `float`; an exact integer square is another number at 16 bits. The
  host recovers the moment with the CPU's two divisions. A change to how
  `moment.c` forms or adds its terms changes the kernel in the same PR.
  `core/test/test_hip_float_moment_exact_contract.py` guards it without a
  device, `test_hip_float_moment_parity` on one (`==`, past 2^53 units too,
  ADR-1497 below).

- **The NEON and SVE2 `float_moment` kernels add in the scalar's order ([ADR-1500](../adr/1500-arm-float-moment-scalar-order.md))**:
  `core/src/feature/arm64/moment_neon.c` and `moment_sve2.c` store each
  vector of samples (squared in `float` for the second moment) and add the
  lanes into one `double` one after the other, as `moment.c` and
  `x86/moment_avx2.c` do; the SVE2 kernel adds the first `svcntp_b32` active
  lanes of a `svwhilelt_b32` predicate and does not depend on the vector
  length. A sync must not bring back lane accumulators, per-row vector sums or
  a vector reduction (`vaddvq_f64`, `svaddv_f64`): past 2^53 units the sum
  rounds on every add. `core/test/test_moment_simd.c` (`==`) guards it; run
  it under `qemu-aarch64` with `sve=off`, `sve128`, `sve256`, `sve512` and
  `sve2048` after touching any of the four kernels.

- **`float_moment_cuda` adds the CPU's float squares ([ADR-1453](../adr/1453-cuda-float-moment-cpu-float-squares.md))**:
  the 16bpc kernel of `core/src/feature/cuda/integer_moment/moment_score.cu`
  adds `moment_float_square()`, one `__fmul_rn()` product of the sample with
  itself converted to an integer, where `moment.c::compute_2nd_moment()` forms
  the square in `float`; an exact integer square is another number at 16 bits.
  The host recovers the moment with the CPU's two divisions. A change to how
  `moment.c` forms or adds its terms changes the kernel in the same PR.
  `core/test/test_cuda_float_moment_exact_contract.py` guards it without a
  device, `test_cuda_float_moment_parity` on one (`==`, past 2^53 units too,
  ADR-1497 below).

- **`float_moment_sycl` adds the CPU's float squares ([ADR-1449](../adr/1449-sycl-float-moment-cpu-float-squares.md))**:
  the kernel of `core/src/feature/sycl/integer_moment_sycl.cpp` adds
  `moment_float_square()`, one fp32 product of the sample with itself
  converted to an integer, where `moment.c::compute_2nd_moment()` forms the
  square in `float`; an exact integer square is another number at 16 bits. The
  host recovers the moment with the CPU's two divisions. A change to how
  `moment.c` forms or adds its terms changes the kernel in the same PR.
  `core/test/test_sycl_float_moment_exact_contract.py` guards it without a
  device, `test_sycl_float_moment_parity` on one (`==`, past 2^53 units too,
  ADR-1497 below).

- **The `float_moment` twins form the CPU's rounded second-moment sum past 2^53 units ([ADR-1497](../adr/1497-float-moment-twins-cpu-sum-past-2-53.md))**:
  on a frame whose sum of float squares can pass 2^53 units
  (`vmaf_moment_sum_may_round()`), the CUDA, SYCL and HIP hosts run four more
  kernels after the frame kernel (row totals, row plans, row units, ordered
  totals) that replace accumulators 2 and 3 with the CPU's sequentially
  rounded sums. The arithmetic and every lane's steps are
  `core/src/feature/float_moment_sum.h` (integers only); the CUDA and HIP
  kernels are `core/src/feature/float_moment_sum_gpu.h`, compiled into
  `moment_score.cu` / `moment_score.hip`; the SYCL kernels are in
  `integer_moment_sycl.cpp` and pick planes by value. A sync must not drop the
  four kernels, add a row from its increments without
  `vmaf_moment_sum_add_run()`'s check, reorder the tree, or bring back the
  exact sum rounded once. A change to `compute_2nd_moment()`'s order or term
  changes the header and `test_float_moment_sum` in the same PR.
  `test_float_moment_sum` (host, the kernels' steps against
  `picture_copy()` + `compute_2nd_moment()` up to 7680x4320) and
  `test_float_moment_sum_contract.py` guard it without a device,
  `test_{cuda,sycl,hip}_float_moment_parity` on one.

## SpEED and CAMBI

- **SpEED evaluates Netflix's fp64 expressions ([ADR-1477](../adr/1477-speed-upstream-double-math.md))**:
  three places of `core/src/feature/speed.c` are fp64 arithmetic rounded to
  `float` once, as Netflix master has them: `1.0 / sqrt(1 + t * t)` in
  `create_givens()`, the `log2()` sum of `update_entropy()` and the `log2()`
  weights of `get_speed_score()`. A sync takes upstream's side on these
  lines and never restores `sqrtf` / `log2f` / `0.75f` (the fork's port #213
  had them: up to 6.6e-4 from Netflix). The GPU twins run `speed.c` on the
  device up to the per-block variances, read one block back per frame
  (`SpeedGpuTailLayout`, `core/src/feature/speed_gpu_common.h`) and form the
  entropies and the score on the host with
  `speed_internal_gpu_tail_scores()` (`core/src/feature/speed_internal.c`),
  which holds those statements; the rotation's statement is
  `speed_givens_unit()` (`core/src/feature/speed_givens.h`) in the kernels.
  A change to `update_entropy()`, `get_speed_score()` or
  `speed_extract_score()` changes the tail in the same PR, and a change to
  `create_givens()` changes `si_create_givens()` and `speed_givens.h`. No
  kernel may evaluate a logarithm or form a score, and the six gate cells
  stay exact (`scripts/ci/exact_twins.d/speed_{chroma,temporal}.*`).
  `core/test/test_speed_upstream_form.c` guards `speed.c`'s three
  functions and the tail against upstream's statements evaluated with the
  host's own `log2()` on every C library, the rotation on every input, and,
  on glibc only, the CPU against Netflix's values, without a device (its
  `_foreign_libm` variant runs the other libcs' path); the three source contract tests and
  `test_{cuda,hip,sycl}_speed_*_parity` (`==`) guard the twins.

- **SYCL SpEED device-resident pipeline ([ADR-1358](../adr/1358-sycl-speed-device-resident-linalg.md))**:
  every SpEED kernel lives in `core/src/feature/sycl/speed_sycl_pipeline.cpp`
  and reproduces `speed.c` operation for operation, up to the variances
  (the entropies and the score are the host's since ADR-1477); like every SYCL feature
  TU the SpEED TUs build with contraction off (`sycl_strict_fp_args`,
  ADR-1367), divide and take square
  roots through `div_rn()` / `sqrt_rn()`, and never wait on the queue
  mid-frame. `core/test/test_sycl_kernel_source_contract.py` guards the
  layout; `scripts/dev/speed_gpu_parity.py --backend sycl` re-checks bit
  parity. See [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **SpEED twin parity fixture ([ADR-1430](../adr/1430-cuda-speed-chroma-log2f-bound.md), [ADR-1452](../adr/1452-hip-speed-chroma-log2f-bound.md))**:
  `core/test/test_cuda_speed_chroma_parity.c` and
  `core/test/test_hip_speed_chroma_parity.c` keep the 960x960 textured
  fixture of `core/test/speed_chroma_twin_parity.h`: a smaller or ramp
  fixture has a singular covariance and never reaches the scoring path. The
  comparison is `==` since ADR-1477; the `LIBM_TWINS` bounds those two ADRs
  introduced (`5e-6`, and `4e-5` for `speed_temporal`, ADR-1460) are gone.

- **CUDA CAMBI and SpEED device-resident ([ADR-1379](../adr/1379-cuda-cambi-device-resident-pipeline.md),
  [ADR-1380](../adr/1380-cuda-speed-device-resident-pipeline.md))**:
  `cambi_cuda`, `speed_chroma_cuda` and `speed_temporal_cuda` read back one
  result block and wait once per frame, in `collect()`; a sync must not bring
  back the host c-values, host pooling, host SpEED linear algebra or a
  mid-frame `cuStreamSynchronize`. SpEED's block is the tail of ADR-1477
  (status words, eigenvalues, variances), from which the host forms the
  entropies and the score after the wait. The host constants come from `cambi.c`
  (`vmaf_cambi_*` helpers in `cambi_internal.h`) and
  `speed_internal_gpu_configure()`, shared with the SYCL twins;
  `speed/speed_score.cu` keeps its `__f*_rn` intrinsics and `--fmad=false`
  (every CUDA fatbin's, ADR-1403).
  `core/test/test_cuda_device_resident_contract.py` guards the design. See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **HIP CAMBI and SpEED device-resident pipelines ([ADR-1378](../adr/1378-hip-cambi-device-resident.md), [ADR-1384](../adr/1384-hip-speed-device-resident.md))**:
  no host stage of `cambi.c` / `speed.c` before the frame's wait and no
  mid-frame wait; one staged upload, one readback, the wait in `collect()`.
  SpEED's readback is the tail block of ADR-1477 and `collect()` forms the
  entropies and the score from it on the host. Per-work-item math lives in
  `integer_cambi/cambi_hip_device.h` and `speed/speed_hip_device.h`, which the
  host replay tests compile; the SpEED kernel TU keeps `-ffp-contract=off
  -fhip-fp32-correctly-rounded-divide-sqrt`. The init-time helpers are
  `cambi.c`'s (`cambi_internal.h`) and `speed_internal_gpu_configure()`,
  shared with SYCL. `core/test/test_hip_device_resident_contract.py` guards
  the layout. See [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

## SSIMULACRA 2

- **`ssimulacra2_hip` returns the CPU's score bit for bit ([ADR-1445](../adr/1445-hip-ssimulacra2-cpu-sum-order.md))**:
  `ssimulacra2_device.hip` evaluates the six per-pixel terms with the CPU's
  fp64 expressions (`ss2h_terms()`, no fp32 pairs) and forms their sums with
  `core/src/feature/ordered_sum.h` in the four kernels of the CUDA twin
  (ADR-1433): 1024-pixel chunks in raster order, lanes composed in lane order,
  a checked walk, term-by-term fallback in pixel order. A change to
  `ssim_map()` / `edge_diff_map()` in `ssimulacra2.c` changes `ss2h_terms()`
  in the same PR. `core/test/test_hip_ssimulacra2_exact_contract.py` and
  `core/test/test_ordered_sum.c` guard it without a device,
  `test_hip_ssimulacra2_parity` (`==`) on one. See
  [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

- **SYCL ssimulacra2 / float_ms_ssim single wait ([ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md))**:
  `ssimulacra2_sycl.cpp` runs the whole frame on the device and reads one
  block of per-scale sums in `collect()`. The exact-fp
  helpers live in `core/src/feature/sycl/sycl_exact_fp.h` and need
  contraction off, which every SYCL feature TU has (ADR-1367).
  `integer_ms_ssim_sycl.cpp` enqueues every scale in `submit()` into its own
  partials span and waits once. `core/test/test_sycl_kernel_source_contract.py`
  guards all of it.

- **`ssimulacra2_sycl` returns the CPU's score bit for bit ([ADR-1446](../adr/1446-sycl-ssimulacra2-cpu-bits.md))**:
  a SYCL kernel has no fp64 type, so the six per-sample terms are the CPU's
  doubles computed in 64-bit integers
  (`core/src/feature/sycl/sycl_ssimulacra2_math.h`), and their sums come from
  `core/src/feature/ordered_sum.h` through its `_bits` forms
  (`core/src/feature/sycl/sycl_ordered_sum.h`): 512-pixel chunks in raster
  order, lanes composed in lane order, a checked walk, term-by-term fallback
  in pixel order. The fp32 pair sums are advice for the walk's plan and never
  a result. `ordered_sum.h` is shared with the CUDA and HIP twins and keeps
  every `double` inside `#ifndef VMAF_ORDSUM_NO_FP64`. A change to
  `ssim_map()` / `edge_diff_map()` in `ssimulacra2.c` changes the math header
  and `reference_terms()` of `core/test/test_sycl_ssimulacra2_math.c` in the
  same PR. `core/test/test_sycl_ssimulacra2_exact_contract.py` guards it
  without a device; `test_sycl_ssimulacra2_math`, `test_sycl_ordered_sum`,
  `test_sycl_ssimulacra2_parity` (`==`) and
  `scripts/dev/speed_gpu_parity.py --backend sycl --feature ssimulacra2` on
  one. See [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **CUDA ssimulacra2 single readback ([ADR-1391](../adr/1391-cuda-ssimulacra2-device-resident.md))**:
  `ssimulacra2_cuda.c` enqueues the whole frame in `submit()` on the picture
  stream and reads one block of per-scale sums in `collect()`; no host compute
  or host wait mid-frame. The device TUs `ssimulacra2_device` and
  `ssimulacra2_blur` build with `--fmad=false` (as every CUDA fatbin does,
  ADR-1403), and
  `ssimulacra2_device.cu` compiles the shared `feature/ssimulacra2_math.h`,
  `ssimulacra2_score.h` and `ssimulacra2_eotf_lut.h` into device code through
  the `VMAF_SS2_FUNC` / `VMAF_SS2_EOTF_LUT_STORAGE` hooks, so an upstream change
  to those helpers must stay valid CUDA device code. The per-pixel SSIM / edge
  terms are fp64, and their sums are the sums of the CPU's loops
  ([ADR-1433](../adr/1433-cuda-ssimulacra2-cpu-sum-order.md)): chunks of
  1024 pixels in raster order, integer increments of the running sum's
  binade composed in pixel order (`feature/ordered_sum.h`, compiled into
  device code through the `VMAF_ORDSUM_*` hooks and tested on the host by
  `core/test/test_ordered_sum.c`), one walk per sum with a term-by-term
  fallback. The terms must stay non-negative or NaN, and a change to
  `ssim_map()` / `edge_diff_map()` in `ssimulacra2.c` changes `ss2c_terms()`
  in the same PR. `core/test/test_cuda_ssimulacra2_parity.c` (`==`),
  `core/test/test_cuda_ssimulacra2_exact_contract.py` and
  `scripts/dev/speed_gpu_parity.py --backend cuda --feature ssimulacra2`
  re-check parity. See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

## SSIM and MS-SSIM

- **`float_ms_ssim_cuda` and `integer_ms_ssim_hip` score every plane `enable_chroma` asks for (`T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06`)**:
  both keep geometry, pyramid and term buffers per plane and run the luma
  pipeline once per scored plane, as `float_ms_ssim.c` does; both declare the
  CPU's four options and provide `float_ms_ssim_cb` / `float_ms_ssim_cr`.
  Plane count and plane size come from
  `core/src/feature/metal/float_ms_ssim_option_semantics.h`. A sync must not
  bring back a fixed `n_planes = 1u`, a luma-only `provided_features` or a
  chroma path with its own arithmetic, and the HIP option stays (HISS-14).
  `test_cuda_float_ms_ssim_parity` and `test_hip_ms_ssim_parity` (`==`) on a
  device, `test_cuda_float_ms_ssim_exact_contract.py` and
  `test_hip_kernel_source_contract.py` without one; gate cell
  `float_ms_ssim_chroma`. See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md) and
  [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

- **`float_ms_ssim_cuda` per-scale sums are the CPU's, in the CPU's order ([ADR-1465](../adr/1465-cuda-float-ms-ssim-raster-order-sum.md))**:
  `ms_ssim_vert_lcs` in `core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`
  stores every window's `l`, `c` and `s` at its raster position and
  `integer_ms_ssim_cuda.c::ms_ssim_scale_sums()` adds the three planes of a
  scale in index order, as `iqa/ssim_tools.c::iqa_ssim()` adds them. A sync
  must not bring back a device reduction of the terms: on the frame of
  `core/test/float_ms_ssim_order_frame.h` per-block sums return the
  neighbouring `float` for `float_ms_ssim_c_scale1`. That header is shared
  with the HIP and SYCL twin tests and its bytes are fixed. Preserve the
  kernel, the host loop,
  `core/test/test_cuda_float_ms_ssim_order.c` and
  `core/test/test_cuda_float_ms_ssim_exact_contract.py` together.

- **`float_ms_ssim_sycl` is the CPU's arithmetic ([ADR-1414](../adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md))**:
  the decimate spells each tap `sycl::fma()` as `ms_ssim_decimate.c` fuses
  it; the window sums and the `l` / `c` / `s` terms come from
  `core/src/feature/sycl/sycl_ssim_terms.h`, shared with `float_ssim_sycl`
  (the window sums as exact fp32 pairs, `l` and `c` as the CPU's doubles in
  64-bit integers); every window's `l`, `c` and `s` of every scale is stored
  unreduced and the host adds them in `iqa_ssim()`'s raster order
  ([ADR-1466](../adr/1466-sycl-float-ms-ssim-raster-sum.md); no reduction may
  return to the twin); the host rounds each per-scale mean to fp32 and
  combines as `ms_ssim.c` does. A change to `ms_ssim_decimate.c`,
  `iqa/convolve.c`, `iqa/ssim_tools.c`, `iqa/ssim_accumulate_lane.h` or
  `ms_ssim.c` changes the header or the twin in the same PR.
  `core/test/test_sycl_ms_ssim_parity.c` (`==` on 18 outputs of 3 frames,
  and two order pairs: seeded noise and
  `core/test/float_ms_ssim_order_frame.h`, a file shared byte for byte with
  the CUDA and HIP tests) and `core/test/test_sycl_kernel_source_contract.py`
  guard it. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **Metal IOSurface import reads NV12 / P010 itself ([ADR-1679](../adr/1679-metal-iosurface-biplanar-import.md))**:
  `core/src/metal/picture_import.mm` plans each plane from the surface's CoreVideo
  pixel format through `core/src/metal/iosurface_layout.h` (bi-planar chroma
  de-interleaved, P010 shifted, other layouts `-ENOTSUP`), and FFmpeg patch `0013`
  imports planes 0, 1 and 2 of both frames and fails on an import error.
  `core/test/test_metal_iosurface_filter_contract.py`, `test_metal_iosurface_layout`
  and `test_metal_iosurface_import_parity` guard it. See
  [core/src/metal/AGENTS.md](../../core/src/metal/AGENTS.md).
- **Metal `float_ms_ssim` option parity ([ADR-1334](../adr/1334-metal-ms-ssim-option-parity.md))**:
  `float_ms_ssim_metal` exposes `enable_db`, `clip_db`, `enable_chroma`, and `enable_lcs`
  matching CPU/SYCL/HIP twins. It emits `float_ms_ssim`, `float_ms_ssim_cb`, and
  `float_ms_ssim_cr` on the GPU, enforces the >= 176 minimum plane dimension at init,
  resolves YUV400P to one plane before chroma validation, and uses the exact
  ceil-subsampled 351x351 YUV420P luma boundary. It wires
  `s->enable_db, s->max_db` into `vmaf_ms_ssim_emit_scores` /
  `vmaf_ssim_emit_score_named`. Device-free contracts in
  `core/test/test_metal_ms_ssim_option_semantics`,
  `core/test/test_metal_ms_ssim_options_contract.py`, and
  `core/test/test_nonfinite_collector_wiring.py` protect this against regression.

- **SYCL `float_ssim` decimation mirrors the CPU's ([ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md))**:
  `float_ssim_sycl` reproduces `ssim.c`'s box low-pass and
  `iqa/decimate.c::iqa_decimate()` bit for bit (int64 fixed-point window sum,
  `KBND_SYMMETRIC`, `picture_copy()` scaling) and sizes its planes with the
  shared `iqa/decimate_dim.h`. A change on the CPU side of that pipeline
  changes `core/src/feature/sycl/integer_ssim_sycl.cpp` in the same PR. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md) and
  [core/src/feature/iqa/AGENTS.md](../../core/src/feature/iqa/AGENTS.md).

- **HIP `float_ssim` decimation mirrors the CPU's ([ADR-1405](../adr/1405-hip-float-ssim-device-decimation.md))**:
  `core/src/feature/hip/float_ssim/ssim_decimate.h` is the window sum of
  `iqa/decimate.c::iqa_decimate()` with `ssim.c`'s box low-pass (int64
  fixed-point sum, `KBND_SYMMETRIC`, `picture_copy()` scaling), compiled by
  the kernel and by `core/test/test_hip_float_ssim_decimate.c`, which holds
  it against `iqa_decimate()` byte for byte. A change on the CPU side of that
  pipeline changes the header in the same PR. See
  [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

- **CUDA `float_ssim` is the CPU pipeline on the device ([ADR-1399](../adr/1399-cuda-float-ssim-device-decimation.md))**:
  `core/src/feature/cuda/integer_ssim/ssim_score.cu` reproduces `ssim.c`'s box
  low-pass and `iqa/decimate.c::iqa_decimate()` (exact int64 window sum, one
  rounding, `KBND_SYMMETRIC`), `iqa/convolve.c`'s fp32 products added to a
  `double` sum in both Gaussian passes, and the ADR-1373 per-pixel combine;
  the host sizes the planes with the shared `iqa/decimate_dim.h`. Its score
  equals the CPU's on every measured frame, and `test_cuda_float_ssim_parity`
  asserts equality. A change on the CPU side of that pipeline changes the
  kernel in the same PR. See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md) and
  [core/src/feature/iqa/AGENTS.md](../../core/src/feature/iqa/AGENTS.md).

- **`integer_ssim_cuda` returns the CPU's `ssim` bit for bit ([ADR-1424](../adr/1424-cuda-ssim-cpu-frame-sum.md))**:
  `integer_ssim.c::calc_ssim()` adds every pixel's term into one double in
  raster order, so `integer_ssim_vert_combine`
  (`core/src/feature/cuda/integer_ssim/integer_ssim_score.cu`) stores the
  terms unreduced and `ssim_cuda.c::issim_frame_sum()` adds the plane it reads
  back in index order. Do not reduce the double terms on the device and do not
  reorder the host loop; the int64 weights may stay a block reduction. A
  change to `ssim_reduce_row_range()` or to the order `calc_ssim()` visits
  pixels changes the kernel's `issim_term()` or the host sum in the same PR.
  `core/test/test_cuda_ssim_exact_contract.py` guards it without a device,
  `test_cuda_ssim_parity` on one; the parity gate compares the twin with
  tolerance 0 (`EXACT_TWINS`, feature `ssim`). See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **`float_ssim_cuda` frame sums are the CPU's, in the CPU's order ([ADR-1464](../adr/1464-cuda-float-ssim-raster-order-sum.md))**:
  the pass-2 kernels of `core/src/feature/cuda/integer_ssim/ssim_score.cu`
  store every window's terms at its raster position and
  `integer_ssim_cuda.c::float_ssim_frame_sum()` /
  `float_ssim_frame_sums_lcs()` add them in index order, as
  `iqa/ssim_tools.c::iqa_ssim()` adds them. A sync must not bring back a
  device reduction of the terms: on the frame of
  `core/test/float_ssim_order_frame.h` a per-block sum returns the
  neighbouring `float`. That header is shared with the HIP and SYCL twin
  tests and its bytes are fixed. Preserve the kernels, the two host loops,
  `core/test/test_cuda_float_ssim_order.c` and
  `core/test/test_cuda_float_ssim_exact_contract.py` together.

- **SYCL `float_ssim` adds the CPU's terms in the CPU's order ([ADR-1463](../adr/1463-sycl-float-ssim-raster-sum.md))**:
  `core/src/feature/sycl/sycl_ssim_terms.h::ssim_double_terms()` forms
  `iqa/ssim_accumulate_lane.h`'s `lv` and `cv` as the CPU's doubles in 64-bit
  integers; `float_ssim_sycl` stores every window's term unreduced and the
  host adds them in raster order, as `iqa/ssim_tools.c::iqa_ssim()` does. No
  reduction may return to the float twin and no host sum may change its
  order: either moves the `float` mean by one step on frames whose terms
  cancel. A change to `ssim_accumulate_lane.h` or to the means of
  `ssim_tools.c` changes the header in the same PR.
  `core/test/test_sycl_float_ssim_exact_contract.py` (device-free) and
  `core/test/test_sycl_float_ssim_parity.c` (`==`, with the constructed pair
  of `core/test/float_ssim_order_frame.h`, a file shared byte for byte with
  the CUDA and HIP tests) guard it. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **`integer_ssim_sycl` returns the CPU's score bit for bit ([ADR-1443](../adr/1443-sycl-ssim-cpu-arithmetic.md))**:
  `core/src/feature/sycl/sycl_integer_ssim_math.h` runs the fp64 operations of
  `integer_ssim.c::ssim_reduce_row_range()`'s per-pixel term, one for one and
  in the reference's order, on values held in 64-bit integers
  (`core/src/feature/sycl/sycl_soft_signed.h`, on `sycl_soft_double.h`). The
  kernel stores the bit pattern of every term unreduced and the host adds the
  plane in `calc_ssim()`'s raster order. A change to that expression in
  `integer_ssim.c` changes the header in the same PR. The twin stays free of
  `float` in its term, of a device reduction of the terms, and of scratch
  memory (SIMD-16 with the 256-entry register file).
  `core/test/test_sycl_integer_ssim_math.c` (host and device),
  `core/test/test_sycl_ssim_exact_contract.py` and
  `core/test/test_sycl_ssim_parity.c` guard it. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

## Motion

- **CUDA RC3 CPU parity ([ADR-1372](../adr/1372-cuda-motion-diff-first-pipeline.md),
  [ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md),
  [ADR-1374](../adr/1374-cuda-integer-tiny-frame-guards.md))**: both CUDA
  motion twins run the diff-first SAD kernel of
  `integer_motion_v2/motion_v2_score.cu` through `integer_motion_sad_cuda.c`;
  an upstream sync must not bring back the blur-each-frame `motion_score.cu`.
  `psnr_cuda`, `integer_ssim_cuda`, `float_ssim_cuda` and `float_motion_cuda`
  carry the CPU option tables and call the CPU's helpers (`psnr_score.h`,
  `vmaf_ssim_max_db()`, `motion_clip()`); `ssim_score.cu::ssim_terms()` mirrors
  the CPU's `l * c * s` rounding point for rounding point, and
  `integer_ssim_score` builds with `--fmad=false` (every CUDA fatbin's,
  ADR-1403) and the CPU's grouping.
  The integer ADM DWT row and tap arithmetic lives in
  `integer_adm/adm_dwt2_rows.h`, and `vif_cuda` falls back to the CPU below 16
  pixels. `float_motion_cuda` emits the CPU's `motion3` (`motion_blend_clip()`).
  The motion SAD, PSNR and moment kernels add one atomic per block (per
  accumulator) and PSNR selects its plane with constant indices
  ([ADR-1392](../adr/1392-cuda-integer-reductions-one-atomic-per-block.md)).
  Details: [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **`float_motion_cuda` adds its SAD in the CPU's order ([ADR-1409](../adr/1409-float-motion-twins-cpu-float-sum.md))**:
  `float_motion.c::compute_motion_simd()` keeps one fp32 running sum per row
  and one over the rows. The twin's `float_motion_row_sad` kernel runs one
  thread per row with a plain left-to-right loop, and the host finishes
  through `core/src/feature/float_motion_sad.h`; the scores are the CPU's bit
  for bit and the parity gate compares them with tolerance 0 (`EXACT_TWINS`).
  A change to the CPU's SAD order, or to `convolution_f32_c_s()`'s tap order,
  changes the kernel and the helper in the same PR.
  `core/test/test_cuda_float_motion_parity.c`,
  `core/test/test_float_motion_sad.c` and
  `core/test/test_cuda_kernel_source_contract.py` guard it.

- **`float_motion_sycl` adds its SAD in the CPU's order ([ADR-1411](../adr/1411-sycl-float-motion-cpu-float-sum.md))**:
  the same contract as the CUDA twin above. `fm_row_sad()` in
  `core/src/feature/sycl/float_motion_sycl.cpp` is one plain left-to-right
  loop per work-item, launched over `sycl::range<1>(height)` at sub-group
  size 16 (ADR-1468), and `collect()` finishes through
  `core/src/feature/float_motion_sad.h`. No group, sub-group or atomic
  reduction may return to the TU, and the blur needs the SYCL strict FP line
  (ADR-1367). `core/test/test_sycl_float_motion_parity.c` (`==`) and
  `core/test/test_sycl_kernel_source_contract.py` guard it; the row kernel
  must stay free of scratch memory (`test_sycl_kernel_scratch`, ADR-1395).
  Since 2026-10-03 the twin also emits `motion3` on the host with the CPU's
  `motion_blend_clip()` and declares `motion_blend_factor` /
  `motion_blend_offset` as the CPU table does
  (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`). A change to how
  `float_motion.c` emits `motion3` (index 0 from the first SAD, the flush
  tail, 0 for one frame) changes `collect_fex_sycl()` / `flush_fex_sycl()` in
  the same PR; `test_sycl_twin_option_parity` compares every output with
  `==`, and the gate's `float_motion` cell lists `motion3`.

- **`motion_five_frame_window` is Netflix's, on the fork's picture ownership ([ADR-1478](../adr/1478-motion-five-frame-window-port.md))**:
  `extract()` and the window of `core/src/feature/integer_motion.c` are
  upstream's statements (`a2b59b77`, `a4a1492d`); a sync takes upstream's side
  for the arithmetic and puts a change to upstream's `flush()` into
  `motion_flush_one()` / `vmaf_motion_window_flush()`
  (`core/src/feature/motion_window.h`), which `integer_motion_v2.c` calls
  too. That file is deleted upstream and kept here. In `core/src/libvmaf.c`
  upstream struct-copies `prev_ref` and `prev_prev_ref` into the extractor
  and zeroes them; the fork hands out counted references
  (`fex_take_prev_refs()` / `fex_release_prev_ref()`, ADR-0778) and rotates
  them in the PREV_REF swap of `feature_extractor.cpp`: keep the fork's side
  of those hunks and take only which frames are kept. Upstream keeps frame
  n-2 for every run; the fork keeps it only while a registered extractor's
  `reads_prev_prev_ref()` answers true (the option is on), so a context
  without one holds what it held before the port, and a preallocated pool
  below four pictures is then refused with `-EINVAL` at registration or at
  `vmaf_preallocate_pictures()`, never left to stall (a deliberate deviation
  that moves no score). A sync must not bring back the unconditional window
  or the unconditional `n_threads * 2 + 2` of `check_picture_pool()`. A sync
  must not bring back a
  `@unittest.skip` on the five-frame or `_hfr` tests under `python/test/`.
  `core/test/test_motion_five_frame_window.c`,
  `test_read_pictures_failure_ownership` and the Netflix golden gate guard
  it. See [core/src/feature/AGENTS.md](../../core/src/feature/AGENTS.md) and
  [core/src/AGENTS.md](../../core/src/AGENTS.md).

- **The CUDA, SYCL and HIP motion twins compute `motion_five_frame_window` ([ADR-1491](../adr/1491-gpu-motion-five-frame-window.md))**:
  with the option each twin of `motion` and `motion_v2` takes its SAD against
  the frame two back (CUDA and `motion_v2_sycl`: a ring of three raw planes;
  HIP: two kept planes; `motion_sycl`: two planes with fixed roles, advanced
  by two device copies behind the graph replay, with the kernel enqueued on
  every frame) and derives `motion2` / `motion3` with the CPU's
  `vmaf_motion_window_flush()` (`core/src/feature/motion_window.h`). The
  `motion_v2` twins hold no copy of the CPU flush. A sync or a cleanup must
  not bring back a twin's own window arithmetic, a
  `VMAF_OPT_FLAG_DEFAULT_ONLY` or `-ENOTSUP` for the option on these six
  twins, or move `motion_sycl`'s plane copies into the recorded graph. A
  change to `min_idx` or to the frame `extract()` differences against in
  `integer_motion.c` / `integer_motion_v2.c` changes the twins' `ring` /
  `depth` in the same PR. `test_{cuda,sycl,hip}_motion_five_frame_window`
  (`==`, fixture `core/test/motion_five_frame_twin_parity.h`) and the exact
  gate cells `motion_mffw` / `motion_v2_mffw` guard it on a device;
  `core/test/test_gpu_option_value_capability_contract.py` and
  `core/test/test_{cuda,sycl,hip}_kernel_source_contract.py` (the twins call
  the function and read no stored score back) without one. The Metal twins do
  not declare the option; the CPU extractor computes it there.

## VIF

- **`float_vif_sycl` returns the CPU's scores bit for bit ([ADR-1422](../adr/1422-sycl-float-vif-cpu-arithmetic.md))**:
  the same contract as the CUDA twin, without an fp64 type. The host takes
  each scale's Gaussian from `vif_get_filter()` and hands it to the kernels by
  value. `core/src/feature/sycl/sycl_float_vif_math.h` is
  `vif_pixel_statistic_s()` and `log2f_approx()` operation for operation; its
  `one_plus_ratio()` evaluates the reference's two fp64 expressions as exact
  fp32 pairs and replays the fp64 operations in integers next to a rounding
  boundary. `vif_row_sums()` adds the terms of a row in one work-item and
  `sum_vif_rows()` adds the rows on the host, both in fp32. A change to
  `vif_get_filter()`, to `VIF_OPT_FAST_LOG2` / `log2f_approx()`, to
  `vif_pixel_statistic_s()` or to `vif_statistic_s()` in `vif_tools.c` changes
  that header in the same PR. `core/test/test_sycl_float_vif_math.c` (host and
  device), `core/test/test_sycl_float_vif_exact_contract.py` and
  `core/test/test_sycl_float_vif_parity.c` guard it; every kernel must stay
  free of scratch memory (`test_sycl_kernel_scratch`, ADR-1395). See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **`vif_sycl` returns the CPU's scores bit for bit ([ADR-1432](../adr/1432-sycl-integer-vif-exact-gain.md))**:
  `core/src/feature/sycl/sycl_integer_vif_math.h` returns the two integers
  `integer_vif.c::vif_accumulate_pixel()` truncates from its fp64 gain
  (`sigma2_sq - g * sigma12` and `g * g * sigma1_sq`), from one integer
  division and, for a sample within the fp64 chain's rounding error of an
  integer, from the reference's fp64 operations replayed in 64-bit integers
  (`core/src/feature/sycl/sycl_soft_double.h`, shared with `float_vif_sycl`).
  The host tail rounds each scale's sums to `float` as
  `vif_store_residuals()` does. A change to those lines of `integer_vif.c`
  (the same lines are in `x86/vif_avx2.c`, `x86/vif_avx512.c` and
  `arm64/vif_neon.c`) changes the header in the same PR. The kernels stay
  free of fp64, of `sycl::mul_hi()` on 64-bit operands (wrong values on an
  Arc A380) and of scratch memory. `core/test/test_sycl_integer_vif_math.c`,
  `core/test/test_sycl_vif_exact_gain_contract.py` and
  `core/test/test_sycl_vif_parity.c` guard it. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **`float_vif_cuda` returns the CPU's scores bit for bit ([ADR-1412](../adr/1412-cuda-float-vif-cpu-arithmetic.md))**:
  the host takes each scale's Gaussian from `vif_get_filter()`, as
  `float_vif.c` does, and hands it to the kernels; no kernel file holds a tap.
  `core/src/feature/float_vif_gpu_common.h` (shared with `float_vif_hip`
  since [ADR-1444](../adr/1444-hip-float-vif-cpu-arithmetic.md); CUDA compiles
  it through `core/src/feature/cuda/float_vif/float_vif_device.h`, which maps
  its operators to the `__fmul_rn()` family) is
  `vif_pixel_statistic_s()` and `log2f_approx()` operation for operation
  (`vif_sigma_nsq` in fp64), `float_vif_row_sums` adds the terms of a row in
  one thread, and `fvif_sum_rows()` adds the rows on the host, both in fp32 as
  `vif_statistic_s()` does. A change to `vif_get_filter()`, to
  `VIF_OPT_FAST_LOG2` / `log2f_approx()`, to `vif_pixel_statistic_s()` or to
  `vif_statistic_s()` in `vif_tools.c` changes that header in the same PR.
  `core/test/test_float_vif_device_math.c` and
  `core/test/test_cuda_float_vif_exact_contract.py` guard it without a device,
  `test_cuda_float_vif_parity` on one; the parity gate compares the twin with
  tolerance 0 (`EXACT_TWINS`). See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **`vif_cuda` reads the CPU's log2 table ([ADR-1462](../adr/1462-cuda-vif-reads-host-log2-table.md))**:
  `core/src/feature/cuda/integer_vif/vif_statistics.cuh` holds the table as
  the module global `vif_cuda_log2_table`, `log2_lookup()` reads it with the
  CPU's mask, and no vif kernel source evaluates a logarithm.
  `integer_vif_cuda.c::init_fex_cuda()` fills it with
  `vif_log2_table_generate()`'s values through
  `vmaf_cuda_vif_upload_log2_table()` before any frame is submitted. When
  upstream changes `vif_statistics.cuh` or `filter1d.cu`, keep the lookup and
  do not bring `log_generate()` back; `filter1d.cu` itself is untouched by
  the fork. `core/test/test_cuda_vif_log2_contract.py` guards it without a
  device, `test_cuda_vif_log2_table` on one.

- **`float_vif_hip` returns the CPU's scores bit for bit ([ADR-1444](../adr/1444-hip-float-vif-cpu-arithmetic.md))**:
  the twin compiles `core/src/feature/float_vif_gpu_common.h` with its default
  operators, which round once only because every HIP kernel is built with
  `hip_strict_fp_args`; `float_vif_score.hip` defines no operator, holds no
  tap and reduces nothing per block. The host takes the taps from
  `vif_get_filter()` and passes `vif_sigma_nsq` as a `double`. A change to the
  shared header is a change to both twins:
  `core/test/test_hip_float_vif_exact_contract.py` and
  `core/test/test_float_vif_device_math.c` guard it without a device,
  `test_hip_float_vif_parity` on one. See
  [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

## ADM

- **`float_adm_sycl` returns the CPU's scores bit for bit ([ADR-1434](../adr/1434-sycl-float-adm-cpu-arithmetic.md))**:
  `core/src/feature/sycl/sycl_float_adm_math.h` is the same arithmetic as the
  CUDA twin's device header, without an fp64 type: the three expressions
  `adm_tools.c` evaluates in `double` (the enhancement gain, the 1/30 product
  and the centre tap's 1/15 product) are exact fp32 pairs, and a result next
  to an fp32 rounding boundary replays the fp64 operations in 64-bit integers
  (`sycl_soft_double.h`). The decouple's quotient is fp32 `n / d`, as the
  reference's `DIVS()` is since ADR-1442; it must not become a product with a
  reciprocal. The header also holds what one work-item of the decouple, term
  and row-sum kernels does; `float_adm_sycl.cpp` only launches them. A row is
  added by one work-item and the rows by the host, both in fp32. The weights,
  the region, the pooling and the floor are the reference's own. A change to
  `adm_decouple_s()`, `adm_csf_s()`, `adm_cm_thresh3x3_s()`,
  `adm_csf_den_scale_s()` or `adm_cm_s()` changes this header and the CUDA
  one in the same PR. `core/test/test_sycl_float_adm_math.c` and
  `core/test/test_sycl_float_adm_exact_contract.py` guard it,
  `test_sycl_float_adm_parity` on a device; the twin is declared exact by
  `scripts/ci/exact_twins.d/float_adm.sycl`. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **Float ADM CSF weights are upstream's float arithmetic ([ADR-1489](../adr/1489-float-adm-barten-upstream-float.md))**:
  `dwt_quant_step()` in `core/src/feature/adm_tools.h` keeps `r`, `temp` and
  `Q` in `float` and raises 10 to the `float` product
  `params->k * temp * temp`, as upstream's `adm_tools.h` does; a sync takes
  upstream's side and keeps the suppression comment. `barten_csf_tools.h`
  forms each product and quotient in `float`, as upstream does, and promotes
  the result with an explicit cast, because the SYCL and Metal twins of
  integer ADM compile the header as C++, where upstream's implicit promotion
  calls the `float` math functions: keep the casts around the results, never
  on an operand. `core/src/feature/metal/float_adm_metal.mm` holds a copy of
  the step and changes with it. With these the only difference between the
  fork's `float_adm` and Netflix's on x86 is the division (ADR-1442).
  `core/test/test_float_adm_csf_upstream.c` (values, the bits of a Netflix
  build, C against C++) and
  `core/test/test_float_adm_csf_upstream_contract.py` (source shapes, the
  Metal copy) guard it; the Netflix golden gate would not notice.

- **Integer ADM quantisation step is upstream's ([ADR-1475](../adr/1475-integer-adm-quant-step-upstream-float.md))**:
  `dwt_quant_step()` in `core/src/feature/integer_adm_kernels.h` raises 10 to
  `params->k * temp * temp`, a `float` product, exactly as upstream's
  `integer_adm.c` does; with a `(double)` on an operand (the form #552
  introduced) every integer ADM score and `vmaf_v0.6.1` leave Netflix's
  values by up to 1.8e-5. A sync takes upstream's side of the statement and
  keeps the suppression comment above it. The SYCL twin holds its own copy
  (`sycl/integer_adm_sycl.cpp`) and changes with it; the Metal twin takes the
  CPU's `adm_csf_factors()` in `metal/integer_adm_metal_host.c` and holds no
  copy. `core/test/test_integer_adm_quant_step.c` (values, with the bits of a
  Netflix build) and `core/test/test_integer_adm_quant_step_contract.py` (both
  copies, and no Metal copy) guard it; the Netflix golden gate would not
  notice.

- **Integer ADM scale-0 masking centre tap ([ADR-1402](../adr/1402-adm-cm-centre-tap-int32.md))**:
  the fork keeps the 1/15 centre tap of the masking threshold in int32 and
  clamps `|x| - thr * 2^shift` to [0, INT32_MAX] in int64, where upstream
  master narrows the tap to int16 and subtracts in 32 bits (the fork's own
  Netflix/vmaf PR #1602, second revision, is not merged upstream). The scalar
  definition is `adm_cm_thresh()` in `core/src/feature/integer_adm_kernels.h`
  and `adm_cm_excess_s0()` in `core/src/feature/adm_cm_accumulator.h`; the
  AVX2, AVX-512, CUDA, HIP, SYCL and Metal twins return its value bit for bit
  and change together with it. A sync must not restore the `(int16_t)` cast,
  the 16-bit sign extension in the vector thresholds, `adm_i16()` on the SYCL
  centre term or `abs(x) - (thr << shift)`. `adm_avx2.c` and `adm_avx512.c`
  no longer carry upstream's macros: their scalar parts are the shared
  kernels. `test_integer_adm_cm_threshold`, `test_integer_adm_simd` and
  `test_gpu_adm_tiny_frames` guard it; the Netflix golden gate must be re-run
  on any change. See [core/src/feature/AGENTS.md](../../core/src/feature/AGENTS.md)
  and the "fix/adm-cm-centre-tap-wrap" entry of
  [rebase-notes](../rebase-notes.md).

- **Integer ADM enhancement gain limit ([ADR-1413](../adr/1413-adm-gain-limit-truncated-double-product.md))**:
  the limited sample is the double product `rst * adm_enhn_gain_limit`
  truncated toward zero, as the scalar kernels in
  `core/src/feature/integer_adm_kernels.h` store it. The AVX2 and AVX-512
  decouple kernels use the truncating conversions (upstream master rounds),
  and the SYCL twin forms the same value in integers with
  `adm_gain_limit_product()` from `core/src/feature/adm_gain_limit.h`. A sync
  must not restore `_mm256_cvtpd_epi32` / `_mm512_cvtpd_epi32` /
  `_mm512_cvtpd_epi64` on the product or a fixed-point limit in the twin.
  `test_integer_adm_simd`, `test_adm_gain_limit` and
  `test_gpu_adm_tiny_frames` guard it. See
  [core/src/feature/AGENTS.md](../../core/src/feature/AGENTS.md) and the
  "fix/adm-decouple-fractional-gain-truncation" entry of
  [rebase-notes](../rebase-notes.md).

- **`adm_cuda` returns the CPU's scores bit for bit ([ADR-1416](../adr/1416-cuda-adm-cpu-row-rounding.md))**:
  `core/src/feature/cuda/integer_adm_cuda.c` includes
  `core/src/feature/integer_adm_kernels.h` and takes its CSF weights
  (`adm_csf_factors()`), its denominator border and shifts
  (`adm_csf_den_ctx_init()`, `i4_adm_csf_den_ctx_init()`) and its per-scale
  scores (`adm_cm_result()`, `adm_csf_den_result()` and their `i4_` forms)
  from it; it defines none of them itself. `integer_adm/adm_csf_den.cu` folds
  one whole row per block through `adm_csf_den_round_row_total()`
  (`adm_cm_accumulator.h`), with the shifts as kernel arguments. A change to
  those CPU routines reaches the twin through the header; a change to how
  the CPU folds a denominator row changes that kernel in the same PR.
  `core/test/test_cuda_adm_exact_contract.py` and `test_adm_cm_row_rounding`
  guard it without a device, `test_cuda_adm_parity` on one; the parity gate
  compares the twin with tolerance 0 (`EXACT_TWINS`). See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **SYCL integer ADM AIM pass ([ADR-1362](../adr/1362-sycl-integer-adm-aim-device-pass.md))**:
  `integer_adm_sycl.cpp` computes aim / adm3 on the device and finalises every
  ADM output in the CPU's float arithmetic (bit-exact with the CPU).
  The decouple quotient is clamped in int64 before narrowing. Details and the
  mirror list for upstream `integer_adm.c` changes:
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **Integer AIM is not clipped, float AIM is ([ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md))**:
  `core/src/feature/integer_adm.c` reports `aim_num / den`
  (`vmaf_adm_scale_ratios()`), `core/src/feature/adm.c` reports
  `MIN(aim_num / aim_den, 1)` (`vmaf_adm_finalize_scores()`), each as its
  upstream file does. The shipped `vmaf_v1.0.16` models read the integer
  `adm3`, which the unclipped AIM takes down to `adm_min_val`. A sync or a
  cleanup must not unify the two unless upstream does; the Netflix golden gate
  has no integer AIM above 1 and would not notice.
  `core/test/test_integer_adm_aim_unclipped.c` pins both sides.

- **`float_adm_hip` returns the CPU's scores bit for bit ([ADR-1458](../adr/1458-hip-float-adm-cpu-arithmetic.md))**:
  it compiles `core/src/feature/float_adm_gpu_common.h`, the arithmetic of
  the CUDA twin (next entry), through
  `core/src/feature/hip/float_adm/float_adm_hip_math.h`, which keeps the
  shared header's plain operators: under the strict FP list of the HIP
  kernels they are the reference's operations, the division included. Do not
  respell them with the `__fmul_rn()` family, do not reduce per wave or
  block, and keep the host on the reference's routines
  (`adm_float_reference.h`) with `adm_frame_size_check()` first in `init`.
  `core/test/test_hip_float_adm_exact_contract.py` guards it without a
  device, `test_hip_float_adm_math` (device arithmetic against the host,
  value by value) and `test_hip_float_adm_parity` on one. See
  [core/src/feature/hip/AGENTS.md](../../core/src/feature/hip/AGENTS.md).

- **`float_adm_cuda` returns the CPU's scores bit for bit ([ADR-1420](../adr/1420-cuda-float-adm-cpu-arithmetic.md))**:
  `core/src/feature/float_adm_gpu_common.h` (shared with `float_adm_hip`;
  `core/src/feature/cuda/float_adm/float_adm_device.h` gives it the CUDA
  device spelling) is the decouple, the
  CSF, the masking threshold and the reduction terms of `adm_tools.c`
  operation for operation (the gain limit and the 1/30 and 1/15 constants in
  fp64, the angle threshold as `(cos^2 * |o|^2) * |t|^2`), and its division is
  the reference's, the IEEE fp32 quotient (`__fdiv_rn()`; see the next entry).
  `float_adm_row_sums` adds each row in one thread and the host adds
  the rows, both in fp32. The weights, the reduced region, the pooling and the
  angle constant come from `adm_tools.c` itself through
  `core/src/feature/adm_float_reference.h`; keep those exports, and keep the
  four reductions of `adm_tools.c` on `adm_pool_bands_s()`. A change to
  `adm_decouple_s()`, `adm_csf_s()`, `adm_cm_thresh3x3_s()`,
  `adm_csf_den_scale_s()` or `adm_cm_s()` changes the device header in the
  same PR. `core/test/test_float_adm_device_math.c` and
  `core/test/test_cuda_float_adm_exact_contract.py` guard it without a device,
  `test_cuda_float_adm_parity` on one; the parity gate compares the twin with
  tolerance 0 (`EXACT_TWINS`). See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **Float ADM divides ([ADR-1442](../adr/1442-float-adm-reference-divides.md))**:
  `core/src/feature/adm_options.h` does not define `ADM_OPT_RECIP_DIVISION`
  and `core/src/feature/adm_tools.c` has one `DIVS()`, the plain quotient,
  with an `#error` if the macro is defined. Upstream Netflix defines the macro
  and multiplies by a reciprocal refined from the processor's `RCPSS`
  estimate, which made the scores depend on the processor. An upstream sync
  that touches either file keeps the fork's side of both hunks: no macro, no
  `rcp_s()`, no `<emmintrin.h>` in `adm_tools.c`. No twin may bring a
  reciprocal estimate, a probe of the host or a table of it back, and no
  CUDA flag may relax the division (`--use_fast_math`, `-prec-div=false`).
  `core/test/test_float_adm_divides_contract.py` scans the reference, every
  `float_adm` file of every backend and `core/src/meson.build`;
  `core/test/test_float_adm_device_math.c` checks the value on inputs where
  the estimate and the quotient differ.

## CIEDE2000

- **`ciede_cuda` runs the CPU's arithmetic ([ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md))**:
  `core/src/feature/cuda/integer_ciede/ciede_device.h` is `ciede.c`'s
  `get_lab_color()` and `ciede2000()` statement for statement: fp64 where the
  reference computes in double, float where it stores in float, every
  float-to-double promotion of a libm argument written out (the kernel is
  C++). The reference's two float products, `c_prime_1 * c_prime_2` and
  `r_sub_t * chroma * hue`, are upstream's and are float products in every
  twin ([ADR-1476](../adr/1476-ciede-upstream-expression.md)); a sync takes
  upstream's side of them and no `(double)` goes in front of either. The kernel stores one float per pixel and
  `ciede_frame_sum()` (`core/src/feature/ciede_frame_sum.h`, one definition
  for the CUDA, SYCL and HIP hosts) adds the read-back plane in raster order. Do not
  introduce float math functions, a device reduction or another form of the
  formula. A change to `get_lab_color()`, `ciede2000()`, `get_r_sub_t()` or
  the order of `extract()`'s sum in `ciede.c` changes that header in the same
  PR. The twin is not bit-identical (glibc's math library against CUDA's);
  the gate bounds it at `1e-9` through `LIBM_TWINS`.
  `core/test/test_ciede_device_math.c` and
  `core/test/test_cuda_ciede_exact_contract.py` guard it without a device,
  `test_cuda_ciede_parity` on one. See
  [core/src/feature/cuda/AGENTS.md](../../core/src/feature/cuda/AGENTS.md).

- **`ciede_sycl` runs the CPU's arithmetic on fp32 pairs ([ADR-1436](../adr/1436-sycl-ciede-cpu-arithmetic.md))**:
  `core/src/feature/ciede_ff_math.h` is the same statements as the
  CUDA twin's `ciede_device.h` for a device without an fp64 type: every fp64
  value is an fp32 pair, every math-library call a function of
  `core/src/feature/ff_math.h`, every `float` of the reference a
  float rounded from the pair at the reference's statement. Both headers are
  backend-neutral and shared with `ciede_hip` (ADR-1448);
  `core/src/feature/sycl/sycl_ciede_math.h` and `sycl_ff_math.h` only name
  the SYCL primitives they are built on. The kernel stores
  one float per pixel and `ciede_frame_sum()` adds the read-back plane in
  raster order. Do not introduce the device's fp32 math functions, a device
  reduction, another form of the formula, or a call the compiler does not
  inline: `ciede_pixel()` is flattened into the kernel because a call frame is
  scratch memory (ADR-1395). The header's constants and tables come from
  `scripts/dev/gen_sycl_ff_math.py`; the tables are read from device memory.
  A change to `get_lab_color()`, `ciede2000()`, `get_r_sub_t()` or the order
  of `extract()`'s sum in `ciede.c` changes this header and the CUDA one in
  the same PR. The twin is not bit-identical (the host's `powf`); the gate
  bounds it at `1e-9` through `LIBM_TWINS`.
  `core/test/test_sycl_ciede_exact_contract.py` guards it without a device,
  `test_sycl_ciede_math` and `test_sycl_ciede_parity` on one. See
  [core/src/feature/sycl/AGENTS.md](../../core/src/feature/sycl/AGENTS.md).

- **`ciede_hip` runs the same fp32-pair statements ([ADR-1448](../adr/1448-hip-ciede-cpu-arithmetic.md))**:
  `core/src/feature/hip/integer_ciede/ciede_score.hip` includes
  `core/src/feature/ciede_ff_math.h` through
  `core/src/feature/hip/integer_ciede/ciede_hip_math.h`, which names the HIP
  primitives (`core/src/feature/ff_pair.h` on plain fp32 operators under the
  strict FP list, `fmaf()`, `sqrtf()`, `cbrtf()`, `expf(0.2f * logf(x))`).
  The device has fp64, but its fp64 math functions cost 17 times the frame
  time; do not bring them back. The kernel stores one float per pixel and the
  host adds the plane with `ciede_frame_sum()`. A change to a shared header
  changes the SYCL twin too: both are re-measured (A380 and gfx1036) in the
  same PR. The gate bounds the cell at `1e-9` (`LIBM_TWINS`), not 0.
  `core/test/test_hip_ciede_exact_contract.py` and `test_hip_ciede_math`
  guard it without a device, `test_hip_ciede_parity` on one.
