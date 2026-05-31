<!-- markdownlint-disable MD060 -->
# ADR-0890: CI concurrency + cost audit follow-up to PR #301

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude Code
- **Tags**: ci, cost

## Context

PR #301 added top-level `concurrency:` blocks (group + `cancel-in-progress: true`)
to three PR-triggered workflows that lacked one — `go-ci.yml`, `rust-ci.yml`,
`docker-image.yml` — and tightened three ONNX-Runtime install steps with
`set -euo pipefail` + `curl -fSL --retry 3`. Its "Out-of-scope" note
explicitly deferred `ffmpeg-integration.yml` ("a candidate for the same
treatment, but kept out of this PR to stay strictly within the task scope;
can land in a follow-up"). The user surfaced the cost issue directly:
"CI minutes are being burned because the one-PR-in-flight rule isn't being
followed — that is actively costing money" (paraphrased).

An audit of `.github/workflows/*.yml` against four cost-control axes
(concurrency groups, paths-filters, caches, matrix minimization) found five
remaining gaps where the in-tree convention exists but had not yet been
applied:

1. `ffmpeg-integration.yml` had no `concurrency:` block. Push + PR triggers
   meant a force-push or PR rebase queued a duplicate matrix run instead of
   cancelling the prior one. Cost per duplicate: ~10-15 min runner-min × 3
   legs (gcc, clang, SYCL).
2. `ffmpeg-integration.yml` had no ccache wrapper. libvmaf + FFmpeg combined
   are ~10 min compile time per leg on a cold runner; ccache typically hits
   60-85 % after warm-up per Research-0089 §3.1 — which the
   `libvmaf-build-matrix.yml` legs already exploit. The FFmpeg clone was also
   full-depth (`git clone -q --branch release/8.1`), wasting 20-40 s on each
   leg even though only the worktree at the tip is needed.
3. `sanitizers.yml` (ASan+UBSan PR-gate, TSan master-push) ran clang-18 debug
   builds with no ccache. Sanitizer builds rebuild fresh every PR push;
   measured ~5-7 min cold today, well-served by the established ccache
   pattern.
4. `security-scans.yml` had no `paths-ignore` on the PR trigger. Doc-only PRs
   fired all of CodeQL C++ (~35 min build + analyze), CodeQL Python, CodeQL
   Actions, Semgrep, Gitleaks, Dependency Review — none of which had any
   security-relevant delta to scan. The weekly cron (`0 6 * * 1`) still
   provides full coverage against master, so doc-only PRs that skip the
   gate are not a coverage gap.
5. `lint-and-format.yml::clang-tidy` paid ~5 min of apt-install + meson-setup
   eson-compile **before** its existing "no C/C++ changes — exit 0"
   short-circuit at the inner `Run clang-tidy on changed files` step. An
   early file-delta probe gated on `*.c` / `*.h` / `*.cpp` / `*.hpp` extension
   lets the install + build steps be skipped entirely on doc-only / Python-only
   PRs.

`docker-publish-production.yml`, `nightly*.yml`, `release-please.yml`,
`scorecard.yml`, `supply-chain.yml`, and the four `upstream-*-watcher.yml`
files were intentionally left without concurrency blocks — they are
release / scheduled / cron workflows where cancelling mid-run is the wrong
behaviour. Same rationale as PR #301.

## Decision

Apply the in-tree cost-control conventions to the five gaps above. No new
patterns introduced; every change mirrors an existing in-tree precedent
(`build.yml` / `libvmaf-build-matrix.yml` ccache + concurrency, the
`paths-ignore` deny-list from `libvmaf-build-matrix.yml` /
`tests-and-quality-gates.yml`, the per-step `if:` gating already used
elsewhere in `lint-and-format.yml`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Land the five gaps as one consolidated CI hygiene PR (chosen) | One review, one CI cycle, one rebase note. All five changes share the same "follow-up to PR #301" framing and decision logic. | Slightly larger diff than five micro-PRs. | Matches the user's "ONE PR active at a time — strict" rule. Five micro-PRs would burn more reviewer time + more CI cycles than the saving justifies. |
| Five micro-PRs (one per workflow) | Each can be reverted independently. | 5× CI cycles, 5× review queue slots, violates the one-PR-in-flight constraint, no shared decision context. | Cost outweighs the (negligible) revert-granularity benefit; the five changes are all CI-YAML hygiene with no source/header/patch impact. |
| Adopt a third-party reusable concurrency action (e.g. `softprops/turnstyle`) | Centralises the concurrency-group shape across all workflows. | New external dependency on the supply chain, no benefit over native `concurrency:`. | The native GitHub Actions `concurrency:` block already does exactly what we need; a third-party action adds attack surface for zero functional gain. |
| Disable the `ffmpeg-integration.yml` SYCL leg entirely (most expensive leg, no GPU on runner) | Saves ~10 min/run unconditionally. | Loses build-time coverage of the SYCL FFmpeg patch (0003) on every PR — exactly the gate that catches patch-series rebase breakage before a downstream FFmpeg consumer trips it. | Build-only coverage of the SYCL patch is the entire point of the leg per ADR-0186 / ADR-0726 follow-up; ccache narrows the cost without losing the signal. |

## Consequences

- **Positive**:
  - Force-push / PR-rebase on `ffmpeg-integration.yml` cancels superseded
    matrix runs instead of queueing dups (mirrors the other 8 PR-triggered
    workflows).
  - ccache on `ffmpeg-integration.yml` + `sanitizers.yml` saves 3-5 min per
    leg after warm-up (Research-0089 §3.1 baseline). `ccache --max-size=400M`
    leaves headroom inside the 10 GB per-repo cache budget.
  - `--depth=1` on the FFmpeg clone saves 20-40 s per leg.
  - `paths-ignore` on `security-scans.yml` skips ~50-60 min of compute on
    every doc-only PR (CodeQL C++ build + analyze dominates).
  - Early file-delta probe in `lint-and-format.yml::clang-tidy` skips ~5 min
    of apt + meson setup on every doc-only / Python-only PR.
- **Negative**:
  - One additional `actions/cache@v5` call per affected workflow (already a
    pinned dep, no new SBOM line).
  - The clang-tidy early-skip duplicates the file-pattern list maintained in
    the later step's exclusion logic. The early probe is intentionally less
    strict (asks "any C/C++ at all?", not "any non-GPU C/C++"); a follow-up
    can fold the two probes if maintenance friction surfaces.
- **Neutral / follow-ups**:
  - The `Coverage Gate` job in `tests-and-quality-gates.yml` already caches
    the ORT GPU `.tgz` (~150 MB) — no change needed there.
  - The Vulkan-wrap packagecache in `libvmaf-build-matrix.yml` is dead code
    now that ADR-0726 dropped Vulkan; cleanup belongs in a separate dead-code
    sweep, not this audit.
  - `cppcheck` in `lint-and-format.yml` was left as-is — it scans the whole
    project (no per-file delta), and its `meson setup + meson compile +
    cppcheck` cost on a doc-only PR is ~5 min total. Not a free win the way
    clang-tidy is; deferred until a `dorny/paths-filter`-style probe is
    introduced project-wide.

## References

- PR #301 (chore(ci): add concurrency groups + shell-strict on curl|tar
  steps) — explicit "Out-of-scope" note deferring `ffmpeg-integration.yml`.
- ADR-0317 — path-filter introduction on `ffmpeg-integration.yml` /
  `docker-image.yml`.
- ADR-0341 — `paths-ignore` deny-list on `libvmaf-build-matrix.yml` /
  `tests-and-quality-gates.yml`.
- Research-0089 §3.1 — ccache hit-rate evidence (60-85 % typical after
  warm-up).
- Source: `req` — direct user direction surfaced in the parent agent task
  brief: "ci is clocked because you dont follow my rules and have more than
  one active not draft pr" + "thats actively wasting my money as well".
