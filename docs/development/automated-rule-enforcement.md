# Automated rule enforcement

CI and git hooks mechanically enforce the fork's process rules, so reviewers
can focus on substance instead of checklist policing. This page lists each rule,
whether it blocks a merge, where it runs and how to fix a failure. The tooling
is tracked by [ADR-0124](../adr/0124-automated-rule-enforcement.md); supporting
research is in
[`docs/research/0002-automated-rule-enforcement.md`](../research/0002-automated-rule-enforcement.md).

Per [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md), CI surfaces
that contributors interact with ship human-readable documentation in the same
PR as the code; this page is that documentation.

## What is enforced

Eight rules, each with the same four things to know: the rule, what triggers a
failure, the opt-out and the fix. Rule numbers below are those of the
[agent hard rules](agent-hard-rules.md).

| ADR | Rule | Enforcement | Where it runs |
| --- | --- | --- | --- |
| [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) | Six deep-dive deliverables per fork-local PR (hard rule 9) | **Blocking** CI check | `rule-enforcement.yml` job `deep-dive-checklist` |
| [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md), [ADR-0167](../adr/0167-doc-drift-enforcement.md) | Docs ship with every user-discoverable surface change (hard rule 7) | **Blocking**, required (`Doc-Substance Gate`) | `rule-enforcement.yml` job `doc-substance-check` |
| [ADR-0106](../adr/0106-adr-maintenance-rule.md) | One ADR per non-trivial decision, written first (hard rule 8) | Advisory CI comment | `rule-enforcement.yml` job `adr-backfill-check` |
| [ADR-0105](../adr/0105-copyright-handling-dual-notice.md) | Every C, C++, CUDA, HIP, Objective-C++ or Metal source ships a copyright header, and every source file of an ADR-1250 language an SPDX line (hard rule 6); exceptions are declared with an expiry | Pre-commit hook | `scripts/ci/check-copyright.sh` via `.pre-commit-config.yaml`; list in [`.config/lint-exceptions.d/`](pre-commit-hooks.md#copyright-and-spdx-hook-and-the-declared-exception-list) |
| [ADR-1250](../adr/1250-eupl-fork-relicense.md) | An `SPDX-License-Identifier` line names the licences the file carries, and never puts the fork's or Netflix's licence over someone else's notice | Pre-commit hook | `scripts/ci/tests/test_spdx_tag_matches_notice.py`, hook `spdx-tag-matches-notice` |
| [ADR-0409](../adr/0409-ffmpeg-patches-surface-gate.md) | A libvmaf public-surface change consumed by `ffmpeg-patches/` carries a patch update (hard rule 11) | **Blocking** CI check | `rule-enforcement.yml` job `ffmpeg-patches-surface-check` |
| [ADR-1135](../adr/1135-ci-twin-drift-gate.md) | Every side of a `.c`/`.cpp` twin pair is compiled; every named source path exists | **Blocking**, required | `lint-and-format.yml` job `twin-drift-check`; see [CI overview](ci.md#twin-drift-gate) |
| [ADR-0165](../adr/0165-state-md-bug-tracking.md), [ADR-0334](../adr/0334-state-md-touch-check-ci-gate.md) | Bug-shaped PRs update `docs/state.md` (hard rule 15) | **Blocking**, required (`docs/state.md Gate`) | `rule-enforcement.yml` job `state-md-touch-check`; see [state.md gates](state-md-gates.md) |

The same workflow also hosts three further required gates that this page does
not detail: `silent-revert-check` (`Silent-Revert Guard`, ADR-1284),
`adr-collision-check` (`ADR Collision Guard`, ADR-0386 and ADR-0628) and
`release-script-contract` (`Release Script Contract`, ADR-1128).

### Blocking or advisory

Blocking versus advisory is deliberate. A rule blocks when its predicate is
mechanically decidable:

- ADR-0108: a checkbox is either ticked or it is not, and referenced files
  either appear in the diff or they do not.
- ADR-0409: either a public-header or `meson_options.txt` symbol that patches
  consume changed and a patch is in the diff (or an opt-out line is in the
  body), or it did not.
- ADR-0100 (since ADR-0167): a path-mapped surface either has a matching
  `docs/` edit or it does not.

Rules that involve human judgement ("is this decision non-trivial enough to
warrant an ADR?") post advisory comments instead of blocking the merge queue.

## ADR-0108: deep-dive deliverables (blocking)

The PR template
([`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md))
carries a six-item checklist under the `## Deep-dive deliverables`
heading. The workflow parses the PR body and, for each item, expects
either a ticked box or an opt-out line.

### What the checker accepts

A **ticked box** mentioning the item:

```markdown
- [x] Research digest under docs/research/ (or "no digest needed: trivial")
- [x] CHANGELOG.md "VMAFX" entry
```

An **opt-out line** using the ADR-0108 opt-out syntax:

```markdown
- no digest needed: trivial
- no alternatives: only-one-way fix
- no rebase impact: workflow-only change
```

The parser is intentionally loose on surrounding punctuation because
reviewers sometimes reword the labels. What it checks:

- `- [x]` or `- [ ]` for the six labels: Research digest, Decision
  matrix, AGENTS.md invariant note, Reproducer / smoke-test command,
  CHANGELOG.md, Rebase note.
- `no <keyword> needed` / `no <keyword> impact` / `no
  rebase-sensitive` where `<keyword>` matches a shorthand for the
  item (`digest`, `alternatives`, `rebase`, `reproducer`, `smoke`,
  `changelog`, `AGENTS`).

### What triggers a hard fail

- A checkbox neither ticked nor opted-out — the job prints
  `::error title=ADR-0108 missing deliverable::<item> is neither
  ticked nor opted-out in the PR description.` and exits non-zero.
- A ticked "Research digest" box without a matching
  `docs/research/NNNN-*.md` in the PR diff.
- A ticked "CHANGELOG" box without `CHANGELOG.md` in the PR diff.
- A ticked "Rebase note" box without `docs/rebase-notes.md` in the
  PR diff.

### Upstream-port exemption

PRs that verbatim-port a Netflix commit are exempt: pure syncs have
no fork-local design choices to record. The workflow skips the check
when:

- The PR title starts with a Conventional Commit `port:` /
  `port(scope):` prefix, **or**
- The branch name starts with `port/`.

If neither applies, the gate runs.

### Dependency-only PR exemption (ADR-1152)

Automated dependency-bump pull requests opened by bots (`renovate[bot]` or
`dependabot[bot]`, or branches matching `renovate/*` or `dependabot/*`) cannot
satisfy the deliverables checklist. When every changed file is an allowed
dependency manifest, lockfile or image-tag surface,
`scripts/ci/classify-dependency-pr.sh` classifies the PR as dependency-only.
The job then emits a GitHub Actions notice and succeeds without running the
deliverables body checks.

The allowed surfaces are:

- `package.json`, `go.mod`, `Cargo.toml`, `pyproject.toml`,
  `requirements*.txt`;
- the root `build-config.env`;
- `Dockerfile*`, `Containerfile` / `Containerfile.*` (any directory),
  `dev/Containerfile`, `docker/**`, `docker-compose*.yml`;
- `deploy/helm/**`, `Chart.yaml`, `Chart.lock`;
- `.pre-commit-config.yaml`, `.github/workflows/**`, `changelog.d/**`.

The `build-config.env` allowance applies only at the repository root,
including updates accompanied by its Dockerfile mirrors. A nested
`pkg/build-config.env` or an unrelated `runtime.env` is outside it. A human
change on a non-bot branch still runs the documentation gates.

If any other path is touched, especially source code under `core/`, `ai/`,
`python/`, `compat/`, `cmd/`, `pkg/`, `internal/`, `bindings/`, `tools/`,
`scripts/`, `docs/` or `model/`, the PR is not exempt and the full
deliverables gate executes.

### Fixing a failing check

1. Open the PR description in the GitHub UI.
2. Tick the missing checkbox, or replace the line with `no <item>
   needed: <reason>` explaining why the deliverable is absent.
3. If a referenced file is missing (research digest, CHANGELOG, or
   rebase note), add it to the branch and push — GitHub re-runs the
   workflow automatically.

The workflow re-runs on every `edited` / `synchronize` event, so
editing the description alone is enough when the referenced files
are already in the diff.

## ADR-0100: doc-substance (blocking)

The job `doc-substance-check` (check name `Doc-Substance Gate`) blocks a PR
that changes a user-discoverable surface without a matching `docs/` edit. It
started as an advisory comment and was promoted to blocking by
[ADR-0167](../adr/0167-doc-drift-enforcement.md), which also tightened it to
require a path-mapped docs hit: an ADR under `docs/adr/` alone does not
satisfy it, because ADRs explain decisions to maintainers, not usage to users.

### What triggers a fail

The job diffs the PR against its base and checks each surface against the docs
path that must change with it:

| Surface touched | Docs path that must change |
| --- | --- |
| `core/include/libvmaf/libvmaf_cuda.h`, `libvmaf_sycl.h` | `docs/api/gpu.md` |
| `core/include/libvmaf/libvmaf_dnn.h` | `docs/api/dnn.md` |
| `core/include/libvmaf/libvmaf.h`, `picture.h`, `model.h` | `docs/api/index.md` |
| `core/src/feature/feature_*.c`, `integer_*.c`; `core/src/feature/x86/`, `arm64/` | `docs/metrics/` |
| `core/src/feature/cuda/` | `docs/metrics/` or `docs/backends/cuda/` |
| `core/src/feature/sycl/` | `docs/metrics/` or `docs/backends/sycl/` |
| `core/tools/cli_parse.c`, `vmaf.c`, `vmaf_bench.c` | `docs/usage/` |
| `core/meson_options.txt` | `docs/development/build-flags.md` |
| `mcp-server/vmaf-mcp/` (`src/`, `pyproject.toml`) | `docs/mcp/` |
| `ai/src/vmaf_train/cli/` | `docs/ai/` |
| `ffmpeg-patches/*.patch` | `docs/usage/` |

The mapping lives in the job itself in `rule-enforcement.yml`; read it there
when a surface is missing from this table.

### Opt-out

Add `no docs needed: REASON` anywhere in the PR description (markdown emphasis
is stripped before matching). It is legitimate for pure internal refactors and
bug fixes with no user-visible delta.

### Exemptions

- A machine-generated release PR is exempt (ADR-1151, ADR-1388):
  `scripts/ci/release-pr-exempt.sh` requires a bot or PAT author with a
  verified release-only diff, so a human cannot disarm the gate by branch name.
- Strictly dependency-only bot PRs classified by
  `scripts/ci/classify-dependency-pr.sh` skip the check (ADR-1152). Bot PRs
  that modify source code outside the manifest allowlist remain subject to it.

### Fix

Add the docs update under the indicated path and push, or add the opt-out line
to the PR description.

## ADR-0106: ADR backfill (advisory)

Also advisory. Flags PRs that touch policy or public-surface paths
without adding a new `docs/adr/NNNN-*.md`:

- `core/include/`
- `meson_options.{txt,toml}`
- `.github/` (any workflow change)
- `docs/principles.md`
- `CLAUDE.md` / `AGENTS.md`
- `.pre-commit-config.yaml`

Bug fixes and refactors in these paths are legitimately ADR-free, so
this stays advisory — the reviewer decides whether a new ADR should
have been written.

## ADR-0105: copyright header (pre-commit)

Runs as a pre-commit hook, not CI. Every `*.c` / `*.h` / `*.cpp` /
`*.cxx` / `*.cc` / `*.hpp` / `*.hxx` / `*.cu` / `*.cuh` staged for
commit must have a `Copyright` line in its first 40 lines.

### What the hook checks

Pure presence. Template correctness — which of ADR-0105's three
templates (Netflix-only, Lusoris+Claude-only, dual notice) is the
right fit for a given file — remains a reviewer judgement. The year
range and the fork-authored-vs-upstream-modified split cannot be
derived from a diff alone, so the hook checks the cheapest
mechanically-decidable property.

### Exclusions

- `subprojects/` — vendored upstream trees
- `core/test/data/` — binary fixtures
- `python/vmaf/resource/` and `python/test/resource/` — upstream
  Netflix training-harness assets that predate the fork
- `*config.h.in` — Meson-templated headers
- `*generated*` — code-generator output

### Bypassing the hook

Do not. Operational rule 6 of [`AGENTS.md`](../../AGENTS.md) forbids
`--no-verify`. If you hit a legitimate case that the hook misclassifies,
add an explicit exclude to
[`.pre-commit-config.yaml`](../../.pre-commit-config.yaml)
in the same PR and cite the reason.

## ADR-0409: ffmpeg-patches surface sync (blocking)

Enforces hard rule 11 of the [agent hard rules](agent-hard-rules.md): every PR
that changes a libvmaf
public-surface symbol consumed by `ffmpeg-patches/*.patch` must
update at least one patch file in the same PR. Without this gate the
rule was reviewer-eyes-only, and a missed update only surfaces at
the next `/sync-upstream` rebase, when context recovery is
expensive.

### How detection works

The script
[`scripts/ci/ffmpeg-patches-surface-check.sh`](../../scripts/ci/ffmpeg-patches-surface-check.sh)
runs in two passes:

1. **Build the consumed set.** Concatenate every patch under
   `ffmpeg-patches/`, extract the union of:
   - `vmaf_<ident>` — public C symbols (`vmaf_init`, `vmaf_close`,
     `vmaf_picture_alloc`, …)
   - `Vmaf<TitleCase>` — public C types (`VmafModelConfig`,
     `VmafLogLevel`, `VmafPicture`, …)
   - `libvmaf_<ident>` — pkg-config feature names (`libvmaf_sycl`,
     `libvmaf_cuda`, …)
   - `--enable-libvmaf-*` — FFmpeg configure flags
2. **Build the diff set.** From the PR's diff against
   `core/include/libvmaf/*.h` and `core/meson_options.txt`,
   extract the same identifier shapes from `+`/`-` lines (single-line
   `//` and `/* … */` comments stripped best-effort).

If the two sets intersect and no `ffmpeg-patches/*.patch` is in the
diff, the gate fails.

### Triggers a hard fail

A symbol like `VmafPicture` appears in both the consumed set and the
diff (because the PR adds, removes, or renames a function that takes
a `VmafPicture *`), and the PR diff contains zero patch files.

### Per-PR opt-out

Add a line to the PR description:

```markdown
no ffmpeg-patches update needed: <reason>
```

Legitimate reasons include:

- Doxygen comment fix that mentions a consumed type but does not
  change its signature or wire-level semantics.
- Pure header-only refactor (e.g. reordering `#include` lines, fixing
  an include-what-you-use violation) that touches a header but no
  symbol patches consume.
- Internal-helper rename behind an existing public surface where the
  public symbol itself stays bit-identical.

### Fixing a failing check

1. Identify which patch under `ffmpeg-patches/` consumes the
   surface that changed. The error output prints the matched
   consumed symbols and flags.
2. Update that patch in the same PR — usually a regenerate via
   the patch's source branch, or a hand-edited `git apply`-able diff.
3. Push. The workflow re-runs on `synchronize`.
4. If the change is genuinely patch-irrelevant, edit the PR body
   to add the `no ffmpeg-patches update needed: <reason>` line —
   the workflow re-runs on `edited`.

### Trade-offs

The detector is intentionally liberal. A diff line `int foo(VmafPicture *p);`
trips it whether `foo` is a fork-local helper or a public entry
point, because `VmafPicture` is in the consumed set. The cost of a
false positive is one extra opt-out line in the PR body; the cost of
a false negative — a real surface change slipping through — is
unbounded archaeology at the next sync. See
[ADR-0409](../adr/0409-ffmpeg-patches-surface-gate.md) §Alternatives
considered for why we picked bash + grep over libclang AST or
ctags-based extraction.

## Running the checks locally

The CI workflow mirrors scripts you can run by hand:

```bash
# Copyright hook on staged files
pre-commit run check-copyright --files path/to/file1.c path/to/file2.h

# All pre-commit hooks on staged files
pre-commit run --files $(git diff --cached --name-only)

# All pre-commit hooks against a PR's changed files
pre-commit run --from-ref origin/master --to-ref HEAD
```

### CI-parity hooks (pre-push)

The `.pre-commit-config.yaml` `local` block carries hooks that
mirror CI lint gates so contributors catch cheap mistakes before a
CI round-trip:

| Hook | Stage | What it checks |
| --- | --- | --- |
| `assertion-density` | pre-push | NASA Power-of-10 §5 — every fork-added C function ≥20 lines has ≥1 `assert()`. Backed by `scripts/ci/assertion-density.sh`. |
| `twin-drift-check` | pre-push | [ADR-1135](../adr/1135-ci-twin-drift-gate.md) — every `.c`/`.cpp` twin side is compiled by some build file (or allowlisted with a reason in `scripts/ci/twin-drift-allowlist.txt`); every source path a `meson.build` / `setup.py` / `*.pyx` names exists. Backed by `scripts/ci/twin-drift-check.sh`; same predicate as the required CI check. |
| `mypy-local` | pre-push | `mypy` over the `ai/` and `scripts/` Python files the branch changed, failing only on findings absent at the merge base (ADR-1261). Files under `ai/src/` run with `--explicit-package-bases`. Required `Python Lint` CI uses the same runner and hash-locked checker (ADR-1310); locally, install the lock or otherwise provide the pinned `mypy`. |
| `semgrep-local` | pre-commit | Project-local rules from `.semgrep.yml` (`--error` exit code on match). Standard rule packs (`p/cert-c-strict`, `p/cwe-top-25`) still run in CI only. |
| `test-semgrep-vendored-scope` | pre-commit | Planted-defect recall for the hook above. `scripts/ci/tests/test_semgrep_vendored_scope.py` plants banned calls at the vendored paths in a throwaway copy of `.semgrep.yml` + `.semgrepignore` and fails if the explicit-file scan (this hook's form) or the whole-tree scan (the `Semgrep` CI job's form) misses them, or if the real vendored files contain any. Run it directly with `python3 -m unittest discover -s scripts/ci/tests -p test_semgrep_vendored_scope.py`. |
| `test-sycl-bench-env` | pre-commit, pre-push | `bash scripts/ci/test-sycl-bench-env.sh` — a hostile `$ONEAPI_PREFIX` must never execute inside `scripts/ci/sycl-bench-env.sh` and must still be sourced as a literal path. Hermetic, no oneAPI needed. |
| `test-dev-mcp-entrypoint-probe` | pre-commit, pre-push | `bash scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh` — the dev container entrypoint's GPU probe runs a program name as argv, never `eval`. Hermetic, no Docker needed. |
| `ffmpeg-patches-apply-check` | pre-commit, pre-push | Replays every patch in `ffmpeg-patches/series.txt` cumulatively against the configured FFmpeg release and refreshes the stack; backed by `scripts/ci/ffmpeg_patch_stack.py --refresh` ([FFmpeg patch automation](ffmpeg-patch-automation.md)). |
| `ffmpeg-patches-surface-check` | CI and local | Hard rule 11: a public-surface change without a matching patch update fails ([ADR-0409](../adr/0409-ffmpeg-patches-surface-gate.md)). Run locally with `BASE_SHA=... HEAD_SHA=... PR_BODY=... bash scripts/ci/ffmpeg-patches-surface-check.sh`. |

Install the hooks once on a fresh clone. The target installs the regular
git-hook dispatchers (`scripts/githooks/install.py`,
[ADR-1241](../adr/1241-worktree-hook-dispatch.md)):

```bash
make install-hooks
```

The ffmpeg-patches gate degrades gracefully when offline: if it
cannot clone or fetch FFmpeg, it prints a stderr warning and exits
0 rather than blocking a local push on connectivity.

The deep-dive-checklist, doc-substance, and adr-backfill jobs run
purely against `git diff --name-only <base>..<head>` and the PR
body, so you can simulate them with `gh pr view --json body` +
`git diff --name-only` if you're curious whether a WIP PR would
pass.

### Stale code-scanning configuration: `security.yml:semgrep`

GitHub's **Settings → Code security → Code scanning → Tools → Semgrep
OSS** page shows a stale configuration pinned to
`.github/workflows/security.yml:semgrep` with a "workflow file no
longer exists" warning. The workflow was renamed `security.yml →
security-scans.yml` in PR #53 (ADR-0116, 2026-04-21 Title-Case
sweep). The current workflow uploads the repository-owned `semgrep-local`
SARIF under `.github/workflows/security-scans.yml:semgrep`, so the required
`Semgrep OSS` check remains live. Per
[ADR-1314](../adr/1314-semgrep-registry-advisory-artifact.md), the moving
registry-pack result is retained for 14 days as the
`semgrep-registry-sarif` workflow artifact instead of entering the required
Code Scanning identity. Only the orphan tool registration lingers.

There is **no public REST endpoint** to delete a code-scanning tool
configuration (only individual analyses via
`DELETE /repos/{owner}/{repo}/code-scanning/analyses/{id}`), and the
original 2026-04-21 analyses have already rolled off the API window.
Cleanup is **manual**: open the Semgrep OSS Tools page and click the
`…` menu in the upper-right → **Delete configuration**. After that
the warning is gone permanently. Do not re-add a `security.yml`
shim — it would introduce a duplicate workflow registration.

## Why this design

- **Single workflow file for the rule jobs.** The gates share the same
  trigger (`on: pull_request`), runner image and toolchain (`grep`, `git`).
  Separate workflow files would duplicate boilerplate for no mental-model
  gain. See the research digest
  ([`docs/research/0002-automated-rule-enforcement.md`](../research/0002-automated-rule-enforcement.md))
  for the alternatives considered.
- **Plain bash, not `danger.js`.** CI is C / Python / meson / bash
  today. Adding a Node runtime purely for PR-body parsing would
  widen the supply-chain surface for no functional win.
- **Advisory-by-default when the rule has human judgement.** An
  earlier draft tried to block `doc-substance-check`; it would have
  blocked the VIF init leak fix on PR #47, which was a legitimate
  no-docs bug fix. Blocking rules need decidable predicates; ADR-0167
  made `doc-substance-check` blocking once a path-mapped predicate and the
  `no docs needed: REASON` opt-out made it decidable.

## Related

- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — doc-substance
- [ADR-0105](../adr/0105-copyright-handling-dual-notice.md) — copyright
- [ADR-0106](../adr/0106-adr-maintenance-rule.md) — ADR backfill
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — six deliverables
- [ADR-0124](../adr/0124-automated-rule-enforcement.md) — this tooling
- [Research-0002](../research/0002-automated-rule-enforcement.md) — supporting
  investigation
