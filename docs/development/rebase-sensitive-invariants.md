<!-- markdownlint-disable MD013 -->
# Rebase-sensitive invariants

Cross-package invariants that any upstream-sync or rebase agent must preserve.
Referenced from the canonical [`AGENTS.md`](../../AGENTS.md) harness. Per-subtree
detail lives in the `AGENTS.md` under each subtree; this page is the index. When a
rebase touches a cited translation unit, read that subtree harness before resolving
conflicts.

Cross-package invariants that any upstream-sync / rebase agent must
preserve. Per-subtree details (the load-bearing reasons + load-bearing
mechanics) live in the relevant `AGENTS.md` under that subtree; this
list is the index. When a rebase touches the cited TUs, walk the
linked AGENTS.md before resolving conflicts.

- **Documentation entry points**: keep `README.md` concise and link to the
  topic guides for changing build requirements, backend coverage and model
  defaults. `docs/index.md` and `docs/backends/index.md` should link to backend
  guides rather than repeat kernel counts or maturity summaries. Keep the
  repository-root build instructions in `docs/getting-started/index.md` and
  include Meson's `core/` source directory when showing a configure command.

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
- **MCP embedded scaffold (T5-2a, ADR-0209)**:
  [ADR-0209](../adr/0209-mcp-embedded-scaffold.md). Public header
  `libvmaf_mcp.h`, audit-first `-ENOSYS` stubs in
  `core/src/mcp/mcp.c`, `enable_mcp` + 3 transport sub-flags. T5-2b
  (cJSON + mongoose + transport bodies) is open. See
  [core/AGENTS.md §Rebase-sensitive invariants](../../core/AGENTS.md).
- **HIP scaffold (T7-10, ADR-0212 placeholder, PR #200)** —
  audit-first AMD HIP backend scaffold. Public `libvmaf_hip.h`,
  19 registered feature extractors + 3 unregistered legacy stubs,
  `enable_hip` meson option default `false`.
- **SVE2 SIMD ports (T7-38, ADR-0213 placeholder, PR #201)** —
  SSIMULACRA 2 PTLR + IIR-blur SVE2 ports developed against
  `qemu-aarch64-static`. Same bit-exact contract as the existing
  NEON ports.
- **GPU-parity CI gate (T6-8, ADR-0214)**:
  [ADR-0214](../adr/0214-gpu-parity-ci-gate.md). Single source of
  truth for cross-backend tolerances:
  `scripts/ci/cross_backend_parity_gate.py`. Adding a new GPU twin
  requires (1) `FEATURE_METRICS` entry, (2) `FEATURE_TOLERANCE` entry
  if it relaxes places=4, (3) row in
  `docs/development/cross-backend-gate.md`. See
  [core/AGENTS.md](../../core/AGENTS.md).
- **FastDVDnet temporal pre-filter (T6-7, ADR-0215 placeholder,
  PR #203)** — 5-frame window pre-filter feeding ssim/ms_ssim.
- **psnr chroma GPU twins (T3-15(b), PR #204)** — `psnr_cb` /
  `psnr_cr` device kernels alongside the existing `psnr_y` from
  [ADR-0182](../adr/0182-gpu-long-tail-batch-1.md). (The original
  Vulkan implementation was removed with the backend in ADR-0726.)
- **MobileSal saliency extractor (T6-2a, ADR-0218 placeholder,
  PR #208)** — first half of T6-2 (encoder-side ROI bundle).
  Saliency-weighted VMAF, sidecar emit for `tools/vmaf-roi`.
- **TransNet V2 shot-boundary extractor (T6-3a, PR #210)** —
  ~1M params; feeds `tools/vmaf-perShot` CRF predictor.
- **SYCL fp64-less device contract (T7-17, ADR-0220)**:
  [ADR-0220](../adr/0220-sycl-fp64-fallback.md). SYCL feature
  kernels are unconditionally fp64-free; a single fp64 instruction
  in any lambda blocks the whole TU on Arc A-series. See
  [core/src/sycl/AGENTS.md](../../core/src/sycl/AGENTS.md).
- **Model registry + Sigstore (T6-9, ADR-0211 placeholder, PR #199)**:
  `--tiny-model-verify` flag + registry schema + Sigstore bundle
  paths. Pairs with
  [ADR-0010](../adr/0010-sigstore-keyless-signing.md) (release
  signing).
- **Upstream port — feature/motion options from b949cebf
  (T-NEW-1)**: PR #197 (`b949cebf`, MERGED 2026-04-29) ported
  Netflix's feature/motion several-options commit; PR #213 (open)
  ports `d3647c73` `feature/speed` extractors (`speed_chroma` +
  `speed_temporal`).

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
  The repo-root `noxfile.py` exposes one session per Python package
  (`ai`, `mcp`, `vmaf_tune`, `dev_llm`, `roi_score`, `ensemble_kit`,
  `python_harness`) plus `all` / `lint` meta-sessions. CI does **not**
  call nox — each package keeps its own `python3 -m venv && pip install
  -e .[dev] && pytest` recipe in
  `.github/workflows/tests-and-quality-gates.yml`. When adding a new
  Python package, update **both** `noxfile.py` and the CI YAML; missing
  one drifts the dev experience away from CI. See
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
