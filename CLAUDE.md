<!-- markdownlint-disable MD013 -->
# CLAUDE.md — VMAF Fork (Lusoris)

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

High-signal orientation for Claude Code sessions opened in this repo.
Non-Claude agents: see [AGENTS.md](AGENTS.md) (same content, tool-agnostic).

## 1. What this repo is

- Fork of [Netflix/vmaf](https://github.com/Netflix/vmaf) — perceptual video quality assessment.
- Additions on top of upstream:
  - SYCL / CUDA / HIP GPU backends (runtime-selected).
  - AVX2 / AVX-512 / NEON SIMD paths.
  - `--precision` CLI flag (default `%.6f` matching upstream golden gate; `--precision=max` opts in to `%.17g` IEEE-754 round-trip lossless — ADR-0119 supersedes ADR-0006).
  - Tiny-AI (ONNX Runtime) model surface — see `ai/`, `core/src/dnn/`.
  - MCP server — see `mcp-server/vmaf-mcp/`.
- License: BSD-2-Clause-Patent (upstream license preserved). See [LICENSE](LICENSE).
- Default branch on this fork: `master`. Upstream is tracked as remote `upstream`.

## 2. How to build

Meson + Ninja (NOT CMake).

```bash
# CPU only (fastest build, no GPU deps required)
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build

# With CUDA (requires /opt/cuda + nvcc)
meson setup build -Denable_cuda=true -Denable_sycl=false
ninja -C build

# With SYCL (requires oneAPI / icpx)
meson setup build -Denable_cuda=false -Denable_sycl=true
ninja -C build

# Full (both backends)
meson setup build -Denable_cuda=true -Denable_sycl=true
ninja -C build
```

Shortcut: `/build-vmaf --backend=cpu|cuda|sycl|all` (skill).

**IDE note**: `${workspaceFolder}/build/` is the dir the
clangd / VS Code C/C++ extension reads from
([`.vscode/c_cpp_properties.json`](.vscode/c_cpp_properties.json)).
Configure it with every backend you have a toolchain for —
otherwise `compile_commands.json` won't contain include paths
for CUDA / SYCL headers and clangd lights up every
backend file with "undeclared identifier" errors. (Vulkan removed
per ADR-0726; `volk.h` / `vk_mem_alloc.h` are no longer needed.)
See [docs/development/ide-setup.md](docs/development/ide-setup.md).

## 3. How to test

```bash
meson test -C build                     # all unit tests
meson test -C build --suite=fast        # fast subset (pre-push gate)
make test                               # full suite + ASan + UBSan
make test-netflix-golden                # the 3 Netflix CPU golden-data tests (see §8)
```

## 4. How to lint

```bash
make lint        # clang-tidy + cppcheck + iwyu + ruff + semgrep
make format      # clang-format + black + ruff (write)
make format-check  # same, no writes (pre-commit / CI)
```

Skills: `/format-all`, `/lint-all`.

## 5. Where the code is

```text
core/                         # C library + build root (was libvmaf/, ADR-0700)
  src/                        # C sources (metric engine, feature extractors)
    feature/                  # per-feature CPU implementations
      x86/                    # AVX2 / AVX-512 SIMD paths
      arm64/                  # NEON SIMD paths
      cuda/                   # CUDA feature kernels
      sycl/                   # SYCL feature kernels
    cuda/                     # CUDA backend runtime (picture, dispatch)
    sycl/                     # SYCL backend runtime (queue, USM, dmabuf)
    dnn/                      # ONNX Runtime integration (tiny AI, Phase 3k)
  include/libvmaf/            # public C API headers (install path unchanged)
  tools/                      # CLI: vmaf.cpp, vmaf_bench.c, cli_parse.cpp
  test/                       # C unit tests
compat/python-vmaf/           # Python harness package (was python/vmaf/, ADR-0700)
python/vmaf/                  # Shim — re-exports from compat/python-vmaf/
python/test/                  # Python tests — contains Netflix golden assertions
ai/                           # Tiny-AI training (Python / PyTorch + Lightning)
mcp-server/vmaf-mcp/          # MCP JSON-RPC server (Python)
model/                        # .json / .pkl / .onnx VMAF models
testdata/                     # YUV fixtures + benchmark JSONs (fork-added)
docs/                         # all documentation (upstream-mirrored + fork-added)
.claude/                      # Claude Code config (skills, agents, hooks)
.workingdir/                  # session audit + gap-fill plans (gitignored; read/write)
.workingdir2/                 # planning dossier — BACKLOG, OPEN, PLAN, decisions (gitignored)
```

## 6. Coding standards

**Read [docs/principles.md](docs/principles.md) before writing C.** Summary:

- NASA/JPL Power of 10 (`.clang-tidy` enforces)
- JPL Institutional Coding Standard for C — applicable subset
- SEI CERT C & CERT C++ — mandatory
- MISRA C:2012 — informative only
- Style: K&R, 4-space, 100-char columns
- Banned functions: `gets`, `strcpy`, `strcat`, `sprintf`, `strtok`, `atoi`, `atof`,
  `rand`, `system` — see `docs/principles.md §1.2 rule 30`.
- Every non-void return value is checked or explicitly `(void)`-discarded.

## 7. When adding new functionality

| Task                                | Skill invocation                              |
|-------------------------------------|-----------------------------------------------|
| New GPU backend (hip, vulkan, metal)| `/add-gpu-backend <name>`                     |
| New SIMD path                       | `/add-simd-path <isa> <feature>`              |
| New feature extractor               | `/add-feature-extractor <name>`               |
| Register a new model JSON           | `/add-model <path>`                           |
| Add/audit AI run manifest           | `/ai-run-manifest <script-or-artifact>`       |
| Profile a hot path                  | `/profile-hotpath <backend> <feature>`        |
| Bisect a regression                 | `/bisect-regression`                          |
| Cross-backend numeric diff          | `/cross-backend-diff`                         |
| Port an upstream commit             | `/port-upstream-commit <sha>`                 |
| Sync with upstream master           | `/sync-upstream`                              |
| Regenerate test snapshots           | `/regen-snapshots` (justification required)   |

## 8. Netflix golden-data gate (do not modify)

The fork preserves Netflix's 3 canonical CPU reference test pairs as the source of truth
for VMAF numerical correctness:

1. Normal: `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`
2. Checkerboard (1-px shift): `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._1_0.yuv`
3. Checkerboard (10-px shift): `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._10_0.yuv`

YUVs: `python/test/resource/yuv/`. Golden assertions: `python/test/quality_runner_test.py`,
`vmafexec_test.py`, `vmafexec_feature_extractor_test.py`, `feature_extractor_test.py`,
`result_test.py` (hardcoded `assertAlmostEqual` values). **Never modify these assertions.**
They run in CI as a required status check. Fork-added tests go in separate files.

## 9. Snapshot regeneration

Fork-added snapshot JSONs under `testdata/scores_cpu_*.json` and
`testdata/netflix_benchmark_results.json` are GPU/SIMD numerical snapshots, NOT Netflix
golden data. Regenerate intentionally via `/regen-snapshots`; include the justification
in the commit message.

## 10. Upstream sync

```bash
git remote add upstream https://github.com/Netflix/vmaf.git  # once
/sync-upstream                                               # creates PR
```

Use `/port-upstream-commit <sha>` for single-commit cherry-picks.

## 11. Release

Automated by `release-please` on pushes to `master`. Version scheme: ordinary
`vMAJOR.MINOR.PATCH`, independent of upstream Netflix releases (ADR-1127
supersedes ADR-0011). Signing is keyless via Sigstore.
Use `/prep-release` to dry-run locally before merging a release PR.

## 12. Hard rules for every session

1. **Never** modify Netflix golden assertions (§8).
2. **Never** `git push --force` to `master`. *(Also host-enforced: branch
   protection rejects force-push and deletion — see
   [ADR-0037](docs/adr/0037-master-branch-protection.md) /
   [release.md](docs/development/release.md#master-branch-protection).)*
3. **Never** commit to `master` directly — branch + PR, merge via squash or ff only.
   *(Also host-enforced: `required_linear_history: true` and 25 required status checks.)*
4. **Never** skip `make lint` before pushing.
5. **Never** commit benchmark output files (`testdata/netflix_benchmark_results.json`
   is usually noise from an ad-hoc run; stash it unless the run is formal).
6. **Every** commit message is Conventional Commits (`type(scope): subject`). Enforced
   by the `commit-msg` git hook.
7. **Every** new `.c` / `.h` / `.cpp` / `.cu` starts with the license header. Use
   `Copyright 2026 Lusoris` for wholly-new files, Netflix
   header for upstream-touched files.
8. **Every** non-trivial architectural, policy, or scope decision gets its own
   ADR file `docs/adr/NNNN-kebab-case.md` following
   [docs/adr/0000-template.md](docs/adr/0000-template.md) **before** the commit
   that implements it lands, plus an index row in
   [docs/adr/README.md](docs/adr/README.md). Non-trivial = another engineer
   could reasonably have chosen differently (directory moves, base-image
   policy, CI-gate semantics, test-selection rules, new dependencies,
   coding-standards changes). Bug fixes and implementation details do NOT
   need an ADR. Cite `req` (direct user quote) or `Q<round>.<q>` (popup
   answer) in the ADR's `## References` section; put the "why" in `## Context`
   and the runner-up options in `## Alternatives considered`. Planning
   dossiers under `.workingdir2/` may mirror ADRs for local continuity, but
   the tracked `docs/adr/` tree is authoritative. **Always run
   `scripts/adr/next-free.sh --claim <slug>` to atomically reserve the next
   ADR number before creating the file** — do not hand-pick a number or use
   the read-only form without `--claim`. The `--claim` flag creates a
   `docs/adr/NNNN-<slug>.md.stub` reservation visible to parallel agents AND
   writes a `.git/adr-claims/<NUMBER>` side-pointer visible to all worktrees
   sharing the same `.git/` directory (so sibling agents in other worktrees
   see the claim immediately, before any push). The allocator also queries
   `git ls-remote --heads origin` to detect in-flight branches that have
   already pushed an ADR file — it returns `max(all-seen) + 1`, biasing
   upward to avoid gaps left by unpushed claims. If the network is
   unreachable, a WARNING is printed to stderr and the allocator falls back
   to local + master + `.git/adr-claims/` only.
   Rename the stub to `.md` when committing. See
   [ADR-0386](docs/adr/0386-adr-numbering-collision-prevention.md),
   [ADR-0535](docs/adr/0535-adr-atomic-allocator.md), and
   [ADR-0628](docs/adr/0628-adr-allocator-remote-aware.md).
9. **Every** session re-reads [docs/adr/README.md](docs/adr/README.md) at
   start and writes missing `docs/adr/NNNN-*.md` files + index rows for any
   decisions inherited from context before the next commit.
10. **Every** PR that adds or changes a user-discoverable surface ships
    **human-readable documentation** under `docs/` in the same PR as the code.
    User-discoverable means: CLI flags or binaries, public C API under
    `core/include/`, feature extractors, GPU backends / SIMD paths,
    `meson_options.txt` build flags, ffmpeg filter options, MCP tools, tiny-AI
    surfaces, and user-visible log / error / output-schema changes. Docs land
    in the existing topic tree — CLI in [`docs/usage/`](docs/usage/), C API in
    [`docs/api/`](docs/api/), extractors in [`docs/metrics/`](docs/metrics/),
    backends in [`docs/backends/`](docs/backends/), build / release in
    [`docs/development/`](docs/development/), tiny-AI in
    [`docs/ai/`](docs/ai/), MCP in [`docs/mcp/`](docs/mcp/), architecture /
    C4 in [`docs/architecture/`](docs/architecture/). The minimum bar is
    *per-surface* (see [ADR-0100](docs/adr/0100-project-wide-doc-substance-rule.md)
    §Per-surface minimum bars). Tiny-AI keeps the tighter 5-point bar in
    [ADR-0042](docs/adr/0042-tinyai-docs-required-per-pr.md) as the
    specialisation. Code comments and ADRs are *not substitutes* — they
    explain decisions to maintainers, not usage to humans. Internal
    refactors, bug fixes with no user-visible delta, and test-only changes
    are excluded.
11. **Every** fork-local PR ships the **six deep-dive deliverables** in
    the same PR (per [ADR-0108](docs/adr/0108-deep-dive-deliverables-rule.md)):
    (1) research digest under [`docs/research/`](docs/research/) (or
    "no digest needed: trivial"), (2) decision matrix in the
    accompanying ADR's `## Alternatives considered` (or "no
    alternatives: only-one-way fix"), (3) `AGENTS.md` invariant note in
    the relevant package (or "no rebase-sensitive invariants"), (4)
    reproducer / smoke-test command in the PR description, (5)
    `changelog.d/<section>/<topic>.md` fragment file per ADR-0221
    (`CHANGELOG.md` itself is rendered by
    `scripts/release/concat-changelog-fragments.sh`), (6) entry in
    [`docs/rebase-notes.md`](docs/rebase-notes.md) (or `no rebase
    impact: REASON`). *Fork-local* means anything not a verbatim port
    of upstream Netflix/vmaf code; pure upstream syncs and
    `port-upstream-commit` PRs are exempt. The PR template
    ([.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md))
    carries the checklist; reviewers verify it.
12. **Every** PR leaves every file it touches **lint-clean** to the
    fork's strictest profile (clang-tidy + cppcheck + the linters in
    `make lint`), regardless of whether the file is fork-local or
    upstream-mirror.
    **Since ADR-1142 the standards apply to the whole tree** —
    upstream-mirror, vendored, GPU kernels (CUDA/HIP/SYCL/Metal), SIMD,
    tests, tools — and the whole tree is bounded by the clang-tidy ratchet
    (`scripts/ci/tidy-ratchet.py` vs
    `scripts/ci/tidy-baseline-<lane>.json`, required CI context
    `Tidy Ratchet`): no file may exceed its baseline
    count, a cleaned file must tighten the baseline in the same PR
    (`make tidy-ratchet-write`), and baselines are never hand-edited.
    "Touches" = any hunk in the PR's diff against its
    merge base. Refactor first (extract helpers, split oversized
    functions, rename reserved identifiers, `(void)`-cast discarded
    return values, ...). `// NOLINT` / `// NOLINTNEXTLINE` is reserved
    for cases where refactoring would break a **load-bearing
    invariant** — e.g., an ADR-0138 / ADR-0139 bit-exactness pattern
    that requires an inline per-lane reduction, or an upstream-parity
    identifier the rebase story depends on keeping verbatim. Every
    NOLINT cites inline the ADR / research digest / rebase invariant
    that forces it; a NOLINT without a justification comment is itself
    a lint violation. Historical debt from before this rule
    (pre-2026-04-21, ~18 `readability-function-size` NOLINTs +
    upstream `_iqa_*` suppressions) was discharged by PR #327
    (refactor pass) and PR #388 (citation closeout, ADR-0278);
    every NOLINT now in tree carries an inline citation. The
    rule does not backdate itself onto in-flight PRs that don't
    touch those files. See
    [ADR-0141](docs/adr/0141-touched-file-cleanup-rule.md) and
    [ADR-0278](docs/adr/0278-nolint-citation-closeout.md).
13. **Every** PR that closes a bug, opens a bug, or rules a Netflix
    upstream report not-affecting-the-fork updates
    [`docs/state.md`](docs/state.md) in the **same PR**. The update
    lands a row in the appropriate section (Open / Recently closed /
    Confirmed not-affected / Deferred) and cross-links the ADR, the
    PR + commit, and the Netflix issue (if any). State drift compounds
    across sessions — the rule trades a 30-second edit for hours of
    re-investigation cost when context resets. The PR template
    ([.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md))
    carries a checkbox; reviewers verify it. See
    [ADR-0165](docs/adr/0165-state-md-bug-tracking.md). Closes
    Issue #20.
14. **Every** PR that touches a libvmaf C-API surface, a CLI flag,
    a `meson_options.txt` entry, a public header, or any other
    interface that the in-tree `ffmpeg-patches/` patches consume
    must update **the relevant patch file in the same PR** — no
    exceptions. The fork ships its FFmpeg integration as a stack
    of patches against `n8.1` (see
    [`ffmpeg-patches/series.txt`](ffmpeg-patches/series.txt));
    when libvmaf adds a new entry point or renames an existing
    one, the patch that wires it through to FFmpeg's filter has
    to follow in the same PR. Otherwise the next person who
    rebases the patch stack inherits a silently-broken build.
    Applies to: new public C-API entry points used by any patch,
    renamed/removed entry points, new `--enable-libvmaf-*`
    configure flags, new `LIBVMAFContext` fields, new
    `vf_libvmaf.c` filter variants, any change to the symbols the
    `enabled libvmaf*` `check_pkg_config` lines probe. Does NOT
    apply to: pure libvmaf-internal refactors that don't change
    public headers, kernel implementations behind an existing
    public surface, doc-only changes, test-only changes. The PR
    template carries a checklist row; reviewers verify by running
    a series replay against a clean `n8.1` checkout
    (`git -C /path/to/ffmpeg-8 reset --hard n8.1 && for p in
    ffmpeg-patches/000*-*.patch; do git -C /path/to/ffmpeg-8 am
    --3way "$p" || break; done`) — per-patch `git apply --check`
    is **the wrong gate** because patches `0002…0006` build on
    each other and standalone-apply cleanly only against the
    cumulative state from earlier patches, not against pristine
    `n8.1`. See
    [ADR-0186](docs/adr/0186-vulkan-image-import-impl.md).
15. **Default to the `vmaf-dev-mcp` container for vmaf / vmaf-tune /
    ai / MCP-probing work.** The container at
    [`dev/Containerfile`](dev/Containerfile) bakes in every backend
    (CUDA + SYCL + HIP + Metal scaffolds), oneAPI, the
    NVIDIA Container Toolkit runtime, ffmpeg with libvmaf, the MCP
    server, and the workspace mount. Host-side `meson setup build`
    chases moving toolchain targets (icpx missing, libsvm wheel drift,
    locale leaks); the container eliminates that entire class of yak
    shave. (The Vulkan backend was removed in ADR-0726.) The rule:
    - **Before any non-trivial vmaf / vmaf-tune / ai / MCP run**:
      rebuild the container if its image predates the last `master`
      sync that touched anything under `core/`, `mcp-server/`,
      `ai/`, `tools/vmaf-tune/`, or `dev/`. One-liner:
      `docker compose --project-directory $(git rev-parse --show-toplevel)
      -f dev/docker-compose.yml build dev-mcp && docker compose
      -f dev/docker-compose.yml up -d`.
    - **Then exec into it** for the actual work:
      `docker exec vmaf-dev-mcp <command>`. The workspace lives at
      `/workspace/`, the vmaf binary at `/usr/local/bin/vmaf` (with
      every backend live), `.corpus/` and `python/test/resource/` are
      mounted, the MCP socket is at `/sockets/vmaf-mcp.sock`.
    - **Skip the container when**: editing only Python harness files
      that don't touch the C surface (unit tests against the wrapper),
      editing only docs / changelog / ADR, or running pure host-side
      git / gh operations.
    - **Don't reinvent host builds** when a backend isn't reproducing
      in the container — diagnose the container first; if it's a real
      container gap, fix the Containerfile rather than the host
      build-flag soup. Host-side builds remain available
      (`build/`, `core/build-cuda`, `core/build-all`) but are
      no longer the default mental model.
    - **Container is canonical for published artifacts (Phase 4b.9,
      ADR-1102).** Release binaries, published container images
      (`ghcr.io/vmafx/vmafx:*`), and CI benchmark/snapshot artifacts
      used downstream must be produced inside the container. Host-side
      builds are diagnostic-only (IDE/clangd, debugger, sanitizer
      sweeps). Never publish a host-built binary as a release artifact.
      See [docs/development/publishing.md](docs/development/publishing.md)
      for rebuild trigger conditions and approved host-side use cases.
    - **Don't multiplex the same device across parallel jobs.** When
      a long-running job (CHUG re-extract, BVI-DVC sweep) is pinned to
      one device (e.g. CUDA / RTX 4090), schedule sibling parallel
      work on a *different* device: Intel Arc via SYCL, AMD via HIP,
      or CPU. The fork's `--backend $name` selector forces exclusive
      backend choice; use it (or `--no_cuda` / `--no_sycl` / etc. for
      negative selection) to pin each parallel run to its own silicon.
      The dev machine has at least CUDA (RTX 4090), Intel Arc (SYCL),
      and CPU — three independent lanes for cross-backend parity sweeps
      while CHUG keeps CUDA hot. (Vulkan backend removed per ADR-0726.)

    See [docs/development/dev-mcp.md](docs/development/dev-mcp.md) for
    the operator guide, including the cross-backend probe harness and
    how to attach the IDE's MCP client. Background: the host-build
    debt drained an entire merge train on 2026-05-17 (Python 3.14 +
    numpy 2.x + libsvm 3.32 + locale-leaked subprocess output, all of
    which the container had already pinned through).

## 13. Interaction style — prefer the popup question tool

When the agent needs the user to decide between options, resolve ambiguity, or pick
priorities, **use `AskUserQuestion` (the in-chat popup) instead of writing a wall of
numbered questions in prose**. Prose questionnaires slow the user down — they have to
scroll, parse, and type a structured reply. The popup lets them click through in
seconds.

Rules of thumb:

- 2–4 focused questions per popup, each with 2–4 concrete option choices (+ auto "Other").
- Mark the recommended option `(Recommended)` as option 1 when one clearly wins.
- Use `preview` fields when comparing code snippets, configs, or mockups side-by-side.
- Reserve prose for explanations that set up the question, not for the question itself.
- Still fine to answer a direct user question in prose — this rule applies to *asking*,
  not to reporting.
