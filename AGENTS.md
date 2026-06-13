<!-- markdownlint-disable MD013 -->
# AGENTS.md — VMAFx

> **CORRECT REPO: `VMAFx/vmafx` (active) — NOT `lusoris/vmaf` (archived)**
> The fork was renamed and transferred. All new work, PRs, and gh commands must
> target `VMAFx/vmafx`. Run `gh repo set-default VMAFx/vmafx` at the start of
> every session before using any `gh` command.

## 🌟 GLOBAL PROJECT RULES (TOP PRIORITY)

These 2 rules apply to ALL agents, ALL tools, and ALL commits — without exception.
They override everything else in this file.

1. **NEVER modify Netflix golden-data assertions** (`python/test/` `assertAlmostEqual` values).
   They are the numerical-correctness ground truth. If scores drift, fix the code — not the assertions.
2. **EVERY user-discoverable surface gets human-readable documentation in the same PR** as the code.
   No docs = unmergeable PR. ADRs and code comments are not substitutes.

Orientation for any coding agent (Cursor, Copilot, Aider, Continue, Cody, Codeium, etc.)
opened in this repo. For Claude Code–specific tooling (skills, hooks), see
[CLAUDE.md](CLAUDE.md) — the same operational content with Claude-specific commands.

## 1. What this repo is

- Fork of [Netflix/vmaf](https://github.com/Netflix/vmaf) — perceptual video quality
  assessment.
- Additions over upstream: SYCL / CUDA / HIP GPU backends, AVX2/AVX-512/NEON SIMD,
  a `--precision` CLI flag (default `%.6f` Netflix-compat; `--precision=max` opts in
  to `%.17g` round-trip lossless — ADR-0119 supersedes ADR-0006), tiny-AI surface
  (ONNX Runtime), MCP server.
- License: BSD-2-Clause-Patent (upstream license preserved — see [LICENSE](LICENSE)).
- Default branch: `master`. Upstream tracked as remote `upstream`.

## 2. Build

Meson + Ninja.

```text
meson setup build [-Denable_cuda=true|false] [-Denable_sycl=true|false]
ninja -C build
```

## 3. Test

```text
meson test -C build                     # unit tests
meson test -C build --suite=fast        # fast subset
make test                               # full + ASan + UBSan
make test-netflix-golden                # Netflix CPU golden-data gate (see §8)
```

## 4. Lint / format

```text
make lint          # clang-tidy + cppcheck + iwyu + ruff + semgrep
make format        # clang-format + black + isort (writes)
make format-check  # dry-run (CI / pre-commit)
```

## 5. Repository layout

See [CLAUDE.md §5](CLAUDE.md) — identical. Briefly:

- `core/src/` — C engine
- `core/src/{cuda,sycl,dnn}/` — GPU / DNN backends
- `core/src/feature/{x86,arm64,cuda,sycl}/` — per-platform feature implementations
- `core/tools/` — `vmaf` CLI + `vmaf_bench`
- `python/vmaf/` + `python/test/` — Python bindings + tests (golden-data here)
- `ai/` — PyTorch tiny-model training
- `mcp-server/` — MCP JSON-RPC server
- `model/` — VMAF models (.json / .pkl / .onnx)
- `testdata/` — fork-added YUV + snapshot JSONs
- `dev/` — dev-MCP Docker container (`Containerfile`, `docker-compose.yml`, `scripts/`)
- `docs/principles.md` — canonical engineering standards

## 6. Coding standards

All C code must conform to:

- NASA/JPL Power of 10 (enforced by `.clang-tidy`)
- JPL Institutional Coding Standard for C (applicable subset)
- SEI CERT C & CERT C++ (mandatory)
- MISRA C:2012 (informative subset)

Banned functions, pointer/loop/alloc restrictions, and the exact list of `.clang-tidy`
checks that codify them are in [docs/principles.md](docs/principles.md).

Style: K&R, 4-space indent, 100-char line budget (see `.clang-format`).

## 7. Conventional entry points for common tasks

These operational workflows are each codified as a Claude skill under `.claude/skills/`.
Agents without slash-command routing should read the corresponding `SKILL.md` and follow
the steps manually.

| Task                              | See                                               |
|-----------------------------------|---------------------------------------------------|
| Build against a backend           | `.claude/skills/build-vmaf/SKILL.md`              |
| Build ffmpeg w/ libvmaf patches   | `.claude/skills/build-ffmpeg-with-vmaf/SKILL.md`  |
| Refresh ffmpeg-patches/ series    | `.claude/skills/refresh-ffmpeg-patches/SKILL.md`  |
| Add new GPU backend               | `.claude/skills/add-gpu-backend/SKILL.md`         |
| Add SIMD path                     | `.claude/skills/add-simd-path/SKILL.md`           |
| Add feature extractor             | `.claude/skills/add-feature-extractor/SKILL.md`   |
| Add a model                       | `.claude/skills/add-model/SKILL.md`               |
| Add/audit AI run manifest         | `.claude/skills/ai-run-manifest/SKILL.md`         |
| Cross-backend numeric diff        | `.claude/skills/cross-backend-diff/SKILL.md`      |
| Validate scores (per-backend ULP) | `.claude/skills/validate-scores/SKILL.md`         |
| Run Netflix bench harness         | `.claude/skills/run-netflix-bench/SKILL.md`       |
| Profile a hot path                | `.claude/skills/profile-hotpath/SKILL.md`         |
| Bisect a code regression          | `.claude/skills/bisect-regression/SKILL.md`       |
| Bisect ONNX checkpoint quality    | `.claude/skills/bisect-model-quality/SKILL.md`    |
| Port upstream commit              | `.claude/skills/port-upstream-commit/SKILL.md`    |
| Sync with upstream master         | `.claude/skills/sync-upstream/SKILL.md`           |
| Regenerate test snapshots         | `.claude/skills/regen-snapshots/SKILL.md`         |
| Regenerate Doxygen / Sphinx docs  | `.claude/skills/regen-docs/SKILL.md`              |
| Format all (clang-format / black) | `.claude/skills/format-all/SKILL.md`              |
| Lint all (clang-tidy / cppcheck)  | `.claude/skills/lint-all/SKILL.md`                |
| Local-LLM commit-msg draft        | `.claude/skills/dev-llm-commitmsg/SKILL.md`       |
| Local-LLM Doxygen docgen          | `.claude/skills/dev-llm-docgen/SKILL.md`          |
| Local-LLM tiny-AI model card      | `.claude/skills/dev-llm-modelcard/SKILL.md`       |
| Local-LLM file review             | `.claude/skills/dev-llm-review/SKILL.md`          |
| Release dry-run                   | `.claude/skills/prep-release/SKILL.md`            |

## 8. Netflix golden-data gate — never modify

The fork preserves three Netflix-authored CPU reference test pairs as the
numerical-correctness ground truth:

1. **Normal** — `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`
2. **Checkerboard 1-px** — `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._1_0.yuv`
3. **Checkerboard 10-px** — `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._10_0.yuv`

YUV files: `python/test/resource/yuv/`. Golden-score assertions are hardcoded as
`assertAlmostEqual(...)` calls in `python/test/`. **These assertions are never modified
by any PR.** They run in CI as a required status check. Fork-added tests live in separate
files and directories.

## 9. Snapshot regeneration

`testdata/scores_cpu_*.json` and `testdata/netflix_benchmark_results.json` are
fork-added GPU/SIMD snapshots — NOT Netflix golden data. If an intentional numerical
change is needed, regenerate them and include the justification in the commit message.
Non-justified changes in these files will be rejected in review.

## 10. Upstream sync

`git remote add upstream https://github.com/Netflix/vmaf.git` (once), then follow
`.claude/skills/sync-upstream/SKILL.md` to open a sync PR. Individual commits go
through `port-upstream-commit`.

## 11. Release

Release-please is triggered by pushes to `master`. Version scheme `v3.x.y-lusoris.N`,
tracking upstream version + a fork suffix. Signing is keyless via Sigstore / GitHub OIDC.

## 12. Hard rules

1. Never modify Netflix golden-score assertions (§8).
2. Never `git push --force` to `master`.
3. Never commit directly to `master` — PR with squash or fast-forward only.
4. Never merge without `make lint` + `make test` green locally.
5. Every commit message is Conventional Commits (`type(scope): subject`) — enforced by
   the `commit-msg` git hook.
6. Every new `.c` / `.h` / `.cpp` / `.cu` file starts with the applicable license
   header (wholly-new fork files: `Copyright 2026 Lusoris`;
   files touching Netflix code: Netflix header preserved).
7. Every PR that adds or changes a user-discoverable surface ships
   **human-readable documentation** under `docs/` in the same PR as the code.
   User-discoverable means: CLI flags or binaries, public C API under
   `core/include/`, feature extractors, GPU backends / SIMD paths,
   `meson_options.txt` build flags, ffmpeg filter options, MCP tools, tiny-AI
   surfaces, and user-visible log / error / output-schema changes. Docs land
   in the existing topic tree (CLI → `docs/usage/`, C API → `docs/api/`,
   extractors → `docs/metrics/`, backends → `docs/backends/`, build/release
   → `docs/development/`, tiny-AI → `docs/ai/`, MCP → `docs/mcp/`,
   architecture → `docs/architecture/`). The minimum bar is *per-surface*
   (see [ADR-0100](docs/adr/0100-project-wide-doc-substance-rule.md)
   §Per-surface minimum bars). Tiny-AI keeps the tighter 5-point bar in
   [ADR-0042](docs/adr/0042-tinyai-docs-required-per-pr.md) as the
   specialisation. Code comments and ADRs are *not substitutes* — they
   explain decisions to maintainers, not usage to humans. Internal refactors,
   bug fixes with no user-visible delta, and test-only changes are excluded.
8. Every non-trivial architectural, policy, or scope decision ships as its own
   ADR file `docs/adr/NNNN-kebab-case.md` following
   [docs/adr/0000-template.md](docs/adr/0000-template.md) **before** the
   commit that implements it lands. The ADR's index row lives in
   `docs/adr/_index_fragments/<NNNN-slug>.md` (one fragment file per ADR;
   the slug is appended to `docs/adr/_index_fragments/_order.txt`).
   `docs/adr/README.md` is regenerated by
   `scripts/docs/concat-adr-index.sh --write` and must not be edited by
   hand — see [ADR-0221](docs/adr/0221-changelog-adr-fragment-pattern.md).
   Non-trivial = another engineer could reasonably have chosen differently.
   Bug fixes and implementation details do not need an ADR. Cite `req`
   (direct user quote) or `Q<round>.<q>` (popup answer) in the ADR's
   `## References` section. **Invariant (ADR-0532): always run
   `scripts/adr/next-free.sh --claim <slug>` to atomically reserve the next
   ADR number before creating the file.** Do not hand-pick a number or use
   the read-only form without `--claim`. The command creates a stub file
   (`docs/adr/NNNN-<slug>.md.stub`) that all concurrent agents treat as
   taken; rename the stub to `.md` when committing. See
   [ADR-0386](docs/adr/0386-adr-numbering-collision-prevention.md) and
   [ADR-0535](docs/adr/0535-adr-atomic-allocator.md).
9. Every fork-local PR ships the **six deep-dive deliverables** in the same
   PR (per [ADR-0108](docs/adr/0108-deep-dive-deliverables-rule.md)):
   (a) research digest under [`docs/research/`](docs/research/) (or
   "no digest needed: trivial"); (b) decision matrix in the accompanying
   ADR's `## Alternatives considered` (or "no alternatives: only-one-way
   fix"); (c) `AGENTS.md` invariant note in the relevant package (or
   "no rebase-sensitive invariants"); (d) reproducer / smoke-test command
   in the PR description; (e) CHANGELOG fragment file under
   `changelog.d/<section>/<topic>.md` — `CHANGELOG.md` itself is rendered
   by `scripts/release/concat-changelog-fragments.sh` per ADR-0221;
   (f) entry in [`docs/rebase-notes.md`](docs/rebase-notes.md) (or
   `no rebase impact: REASON`). *Fork-local* means anything not a verbatim
   port of upstream Netflix/vmaf code; pure upstream syncs and
   `port-upstream-commit` PRs are exempt. The PR template
   ([.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md))
   carries the checklist.
10. Every PR leaves every file it touches **lint-clean** to the fork's
    strictest profile (clang-tidy + cppcheck + `make lint`), whether the
    file is fork-local or upstream-mirror. "Touches" = any hunk in the
    PR's diff against its merge base. Refactor first; `// NOLINT` is
    reserved for cases where refactoring would break a load-bearing
    invariant (ADR-0138 / ADR-0139 bit-exactness pattern,
    upstream-parity identifier the rebase story depends on). Every
    NOLINT cites inline the ADR / research digest / rebase invariant
    that forces it. Historical pre-2026-04-21 NOLINTs are scoped to
    backlog item T7-5 (one sweep-PR). See
    [ADR-0141](docs/adr/0141-touched-file-cleanup-rule.md).
11. Every PR that touches a libvmaf public surface (C-API entry
    points, public headers, CLI flags, `meson_options.txt`
    entries, or any symbol probed by the `enabled libvmaf*`
    `check_pkg_config` lines) updates the relevant
    `ffmpeg-patches/000*-*.patch` file in the **same PR**. The
    fork ships FFmpeg integration as a patch stack against
    `n8.1`; libvmaf-side surface drift breaks the patches
    silently for the next rebase. Verify with a series replay
    against a clean `n8.1` checkout
    (`git -C ffmpeg-8 reset --hard n8.1 && for p in
    ffmpeg-patches/000*-*.patch; do git -C ffmpeg-8 am
    --3way "$p" || break; done`) — per-patch
    `git apply --check` is the wrong gate (patches build on
    each other). Pure libvmaf
    internals (kernel impls, refactors that don't change
    headers), doc-only changes, and test-only changes are
    exempt. See
    [ADR-0186](docs/adr/0186-vulkan-image-import-impl.md).
12. **Default to the `vmaf-dev-mcp` container for vmaf / vmaf-tune /
    ai / MCP-probing work.** The container at
    [`dev/Containerfile`](dev/Containerfile) bakes in every backend
    (CUDA + SYCL + Vulkan + HIP + Metal scaffolds), oneAPI, NVIDIA
    Container Toolkit runtime, ffmpeg with libvmaf, MCP server, and
    workspace mount. Host-side `meson setup build` chases moving
    toolchain targets (icpx missing, Vulkan SDK gaps, libsvm wheel
    drift, locale leaks); the container eliminates that whole class.
    - **Before any non-trivial vmaf / vmaf-tune / ai / MCP run**:
      rebuild the container if its image predates the last `master`
      sync that touched anything under `core/`, `mcp-server/`,
      `ai/`, `tools/vmaf-tune/`, or `dev/`. One-liner:
      `docker compose --project-directory $(git rev-parse --show-toplevel)
      -f dev/docker-compose.yml build dev-mcp && docker compose
      -f dev/docker-compose.yml up -d`.
    - **Then exec into it** for the actual work:
      `docker exec vmaf-dev-mcp <command>`. Workspace at `/workspace/`,
      vmaf binary at `/usr/local/bin/vmaf` (every backend live),
      `.corpus/` and `python/test/resource/` mounted, MCP socket at
      `/sockets/vmaf-mcp.sock`.
    - **Skip the container when**: editing only Python harness files
      that don't touch the C surface, editing only docs / changelog /
      ADR, or running pure host-side git / gh operations.
    - **Don't reinvent host builds** when a backend isn't reproducing
      in the container — diagnose the container first; fix the
      Containerfile rather than the host build-flag soup. Host-side
      builds remain available (`build/`, `core/build-cuda`,
      `core/build-all`) but are no longer the default mental model.
    - **Don't multiplex the same device across parallel jobs.** When
      a long-running job (CHUG re-extract, BVI-DVC sweep) is pinned
      to one device (e.g. CUDA), schedule sibling parallel work on a
      different device — Intel Arc via SYCL, AMD via HIP, Vulkan on
      a non-NVIDIA adapter, or CPU. Use `--backend $name` (exclusive)
      or `--no_<backend>` (negative) to pin each parallel run to its
      own silicon.

    See [docs/development/dev-mcp.md](docs/development/dev-mcp.md) for
    the operator guide.

## 12a. Worktree discipline ([ADR-0332](docs/adr/0332-agent-worktree-drift-hard-guard.md))

Background coding agents on this fork run in isolated git worktrees
under `.claude/worktrees/agent-<id>/`. Two layers keep them from
clobbering the main checkout:

1. **Process side (you, the agent)** — start in, stay in, and commit
   from your assigned worktree. The canonical pattern at session
   start:

   ```bash
   pwd | grep -q '\.claude/worktrees/' || {
       echo "DRIFT: cwd is not inside an agent worktree" >&2
       exit 1
   }
   AGENT_WT="$(pwd)"
   git -C "$AGENT_WT" status   # always pass -C
   ```

   Use absolute paths and `git -C "$AGENT_WT"` for every git call.
   `cd` does not survive between bash calls in some harnesses;
   `git -C` does. Verify
   `git rev-parse --show-toplevel` equals `$AGENT_WT` every ~20 tool
   uses; stop and ask if it doesn't. Cited in user-scope memory as
   `feedback_agents_isolated_worktree_only` ("never spawn parallel
   agents in the shared tree").

2. **Host side (the pre-commit hook)** —
   `scripts/ci/check-agent-worktree-drift.sh` runs at commit time
   (installed by `make hooks-install`, wired through
   `.pre-commit-config.yaml`). It refuses any commit whose toplevel
   is the main checkout while sibling agent worktrees exist. Bypass
   for legitimate human main-checkout commits while an agent runs:
   `git commit --no-verify`. **Do not bypass when you are the agent
   the guard fired against** — that is the exact pattern it catches.
   `cd` (or `git -C`) into your worktree and re-run.

Full documentation:
[`docs/development/agent-worktree-discipline.md`](docs/development/agent-worktree-discipline.md).

## 13. Rebase-sensitive invariants (project-wide)

Cross-package invariants that any upstream-sync / rebase agent must
preserve. Per-subtree details (the load-bearing reasons + load-bearing
mechanics) live in the relevant `AGENTS.md` under that subtree; this
list is the index. When a rebase touches the cited TUs, walk the
linked AGENTS.md before resolving conflicts.

- **GPU long-tail terminus reached** — every registered feature
  extractor has at least one GPU twin (lpips remains ORT-delegated
  per [ADR-0022](docs/adr/0022-inference-runtime-onnx.md)).
  Cross-backend tolerances live in
  `scripts/ci/cross_backend_parity_gate.py`. Governing ADRs:
  [ADR-0182](docs/adr/0182-gpu-long-tail-batch-1.md) (batch 1: psnr /
  ciede / moment), [ADR-0188](docs/adr/0188-gpu-long-tail-batch-2.md)
  (batch 2: ssim / ms_ssim / psnr_hvs),
  [ADR-0192](docs/adr/0192-gpu-long-tail-batch-3.md) (batch 3:
  motion_v2 / float-twins / ssimulacra2 / cambi; `float_ansnr` removed
  in commit 70ed8b3ce3 / PR #38).
  See [core/src/feature/AGENTS.md](core/src/feature/AGENTS.md).
- **Vulkan backend (scaffold + image import)**:
  [ADR-0175](docs/adr/0175-vulkan-backend-scaffold.md) +
  [ADR-0184](docs/adr/0184-vulkan-image-import-scaffold.md) +
  [ADR-0186](docs/adr/0186-vulkan-image-import-impl.md). Public
  symbols in `libvmaf_vulkan.h`. Volk-symbol hiding via
  [ADR-0185](docs/adr/0185-vulkan-hide-volk-symbols.md) +
  [ADR-0198](docs/adr/0198-volk-priv-remap-static-archive.md) +
  [ADR-0200](docs/adr/0200-volk-priv-remap-pkgconfig-leak-fix.md).
  See [core/src/vulkan/AGENTS.md](core/src/vulkan/AGENTS.md).
- **ssim / ms_ssim Vulkan kernels**:
  [ADR-0188](docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0189](docs/adr/0189-ssim-vulkan.md) +
  [ADR-0190](docs/adr/0190-ms-ssim-vulkan.md). 11-tap Gaussian baked
  into GLSL byte-for-byte from `iqa/ssim_tools.h::g_gaussian_window_h`.
- **motion_v2 GPU port (T3-14)**:
  [ADR-0193](docs/adr/0193-motion-v2-vulkan.md) — single-dispatch GLSL
  with edge-replicating mirror that diverges from `motion.comp`'s
  non-replicating variant; bit-exact vs CPU.
- **cambi Vulkan integration (T7-36, ADR-0210)**:
  [ADR-0210](docs/adr/0210-cambi-vulkan-integration.md) — Strategy II
  hybrid host/GPU; closes ADR-0192 long-tail terminus. See
  [core/src/feature/AGENTS.md](core/src/feature/AGENTS.md).
- **MCP embedded scaffold (T5-2a, ADR-0209)**:
  [ADR-0209](docs/adr/0209-mcp-embedded-scaffold.md). Public header
  `libvmaf_mcp.h`, audit-first `-ENOSYS` stubs in
  `core/src/mcp/mcp.c`, `enable_mcp` + 3 transport sub-flags. T5-2b
  (cJSON + mongoose + transport bodies) is open. See
  [core/AGENTS.md §Rebase-sensitive invariants](core/AGENTS.md).
- **HIP scaffold (T7-10, ADR-0212 placeholder, PR #200)** —
  audit-first AMD HIP backend scaffold mirroring Vulkan T5-1 /
  ADR-0175. Public `libvmaf_hip.h`, stub kernels, `enable_hip` meson
  option default `false`.
- **SVE2 SIMD ports (T7-38, ADR-0213 placeholder, PR #201)** —
  SSIMULACRA 2 PTLR + IIR-blur SVE2 ports developed against
  `qemu-aarch64-static`. Same bit-exact contract as the existing
  NEON ports.
- **GPU-parity CI gate (T6-8, ADR-0214)**:
  [ADR-0214](docs/adr/0214-gpu-parity-ci-gate.md). Single source of
  truth for cross-backend tolerances:
  `scripts/ci/cross_backend_parity_gate.py`. Adding a new GPU twin
  requires (1) `FEATURE_METRICS` entry, (2) `FEATURE_TOLERANCE` entry
  if it relaxes places=4, (3) row in
  `docs/development/cross-backend-gate.md`. See
  [core/AGENTS.md](core/AGENTS.md).
- **FastDVDnet temporal pre-filter (T6-7, ADR-0215 placeholder,
  PR #203)** — 5-frame window pre-filter feeding ssim/ms_ssim.
- **psnr chroma Vulkan (T3-15(b), ADR-0216 placeholder, PR #204)**
  — `psnr_cb` / `psnr_cr` Vulkan kernels alongside the existing
  `psnr_y` from [ADR-0182](docs/adr/0182-gpu-long-tail-batch-1.md).
- **MobileSal saliency extractor (T6-2a, ADR-0218 placeholder,
  PR #208)** — first half of T6-2 (encoder-side ROI bundle).
  Saliency-weighted VMAF, sidecar emit for `tools/vmaf-roi`.
- **TransNet V2 shot-boundary extractor (T6-3a, PR #210)** —
  ~1M params; feeds `tools/vmaf-perShot` CRF predictor.
- **SYCL fp64-less device contract (T7-17, ADR-0220)**:
  [ADR-0220](docs/adr/0220-sycl-fp64-fallback.md). SYCL feature
  kernels are unconditionally fp64-free; a single fp64 instruction
  in any lambda blocks the whole TU on Arc A-series. See
  [core/src/sycl/AGENTS.md](core/src/sycl/AGENTS.md).
- **Model registry + Sigstore (T6-9, ADR-0211 placeholder, PR #199)**:
  `--tiny-model-verify` flag + registry schema + Sigstore bundle
  paths. Pairs with
  [ADR-0010](docs/adr/0010-sigstore-keyless-signing.md) (release
  signing).
- **Upstream port — feature/motion options from b949cebf
  (T-NEW-1)**: PR #197 (`b949cebf`, MERGED 2026-04-29) ported
  Netflix's feature/motion several-options commit; PR #213 (open)
  ports `d3647c73` `feature/speed` extractors (`speed_chroma` +
  `speed_temporal`).

- **Coverage Gate ratchet + per-PR delta gate (ADR-0922)**:
  [ADR-0922](docs/adr/0922-coverage-ratchet-aggressive.md). Absolute
  floors live in `scripts/ci/coverage-check.sh`
  (`OVERALL_MIN=70`, `CRITICAL_MIN=90`, `PER_FILE_MIN[...]`); per-PR
  drop tolerance lives in `scripts/ci/coverage-delta-check.sh`
  (default 0.5pp on overall and per-touched-file). Lowering any
  floor or loosening the delta tolerance requires a new ADR
  superseding ADR-0922. The Coverage Gate job in
  `.github/workflows/tests-and-quality-gates.yml` invokes both
  scripts; the delta gate needs `actions/checkout` with
  `fetch-depth: 0` because it runs `git merge-base`. See
  [scripts/ci/AGENTS.md](scripts/ci/AGENTS.md) §Coverage Gate
  ratchet for the full coupling.
- **CI action pins — Windows MSVC dev env**
  ([ADR-0635](docs/adr/0635-ci-warning-omnibus-2026-05-19.md)):
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
  ([ADR-0435](docs/adr/0435-local-dev-mcp-container.md)):
  `dev/Containerfile` pins `cuda-toolkit-12-6`, `intel-basekit-2025.3`,
  and ROCm 6.x apt repos. If SDK versions are bumped (routine security
  maintenance), update the version pins and the apt repo URL paths in
  `dev/Containerfile` before merging.
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
  [`docs/development/python-test-orchestrator.md`](docs/development/python-test-orchestrator.md).
  The `python_harness` session intentionally delegates to `tox -c
  python` rather than duplicating the Cython + Netflix golden-data
  setup that lives in `python/tox.ini`; do not collapse them.

## 14. Interaction style — prefer structured popup questions

When your host agent exposes a structured-question UI (Claude Code's `AskUserQuestion`,
Cursor's choice prompt, Aider's multi-choice, etc.), **use it instead of posting a
wall of numbered questions in prose**. The user clicks through options in seconds;
prose questionnaires force them to scroll, parse, and type structured replies.

Rules of thumb:

- 2–4 focused questions per round, 2–4 concrete options each.
- Mark the recommended option `(Recommended)` when one clearly wins.
- Reserve prose for setting up the question, not for the question itself.
- Still fine to answer in prose — this rule applies to *asking*, not to reporting.
