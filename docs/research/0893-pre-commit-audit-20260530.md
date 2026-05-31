<!-- markdownlint-disable MD060 -->
# Research digest — Pre-commit config audit (2026-05-30)

**Companion ADR**: [ADR-0893](../adr/0893-pre-commit-audit-2026-05-30.md)
**Branch**: `chore/pre-commit-audit`
**Audited file**: [`.pre-commit-config.yaml`](../../.pre-commit-config.yaml)
**Working tree**: `/tmp/wt-precommit` (worktree on `origin/master` @ `544299fae1`)

## 1. Audit method

1. Read every `repo:` / `local` block in `.pre-commit-config.yaml`,
   listed each `id:` with its pinned revision and any `args:` /
   `exclude:` / `files:` / `stages:` overrides.
2. For upstream repos: ran `pre-commit autoupdate` against a clean
   worktree, captured proposed bumps, then independently verified
   each via `git ls-remote --tags --refs <repo>` because autoupdate
   has a known sort-order bug on repos that release point versions
   out of branch order.
3. For `local` hooks: verified each `entry:` resolves to a real,
   executable script (`scripts/ci/*.sh`, `scripts/git-hooks/*.sh`)
   and that any `language: system` hook's binary is present on the
   dev box (`mypy`, `semgrep`, `gh`).
4. Re-ran `pre-commit run --all-files` against the edited config to
   confirm no new violations.

## 2. Inventory + drift per hook

| Repo / id | Pinned rev | Latest stable | Δ class | Action |
|---|---|---|---|---|
| `pre-commit/pre-commit-hooks/trailing-whitespace` | v6.0.0 | v6.0.0 | none | keep |
| `pre-commit/pre-commit-hooks/end-of-file-fixer` | v6.0.0 | v6.0.0 | none | already present (one of user-listed hygiene hooks) |
| `pre-commit/pre-commit-hooks/check-merge-conflict` | v6.0.0 | v6.0.0 | none | already present (PR #182 wired it) |
| `pre-commit/pre-commit-hooks/check-yaml` | v6.0.0 | v6.0.0 | none | already present |
| `pre-commit/pre-commit-hooks/check-json` | v6.0.0 | v6.0.0 | none | keep |
| `pre-commit/pre-commit-hooks/check-toml` | v6.0.0 | v6.0.0 | none | keep |
| `pre-commit/pre-commit-hooks/check-added-large-files` | v6.0.0 | v6.0.0 | none | keep (maxkb=1024) |
| `pre-commit/pre-commit-hooks/mixed-line-ending` | v6.0.0 | v6.0.0 | none | already present |
| `pre-commit/pre-commit-hooks/detect-private-key` | v6.0.0 | v6.0.0 | none | already present |
| `pre-commit/pre-commit-hooks/forbid-new-submodules` | — (absent) | v6.0.0 | **missing** | **add** — fork uses Meson wraps + `ffmpeg-patches/`, never `.gitmodules`; supply-chain guard |
| `pre-commit/mirrors-clang-format/clang-format` | v22.1.5 | v22.1.5 | none | keep |
| `psf/black` | 26.5.1 | 26.5.1 | none | keep |
| `PyCQA/isort/isort` | 5.13.2 | 8.0.1 (latest); 6.0.1 (conservative) | **stale** | **bump 5.13.2 → 6.0.1** (autoupdate proposed `9.0.0a3` alpha; rejected) |
| `astral-sh/ruff-pre-commit/ruff-check` | v0.15.13 | v0.15.15 | patch behind | **bump v0.15.13 → v0.15.15** |
| `scop/pre-commit-shfmt/shfmt-src` | v3.13.1-1 | v3.13.1-1 | none | keep |
| `shellcheck-py/shellcheck-py/shellcheck` | v0.11.0.1 | v0.11.0.1 | none | keep |
| `gitleaks/gitleaks/gitleaks` | v8.30.1 | v8.30.1 | none (autoupdate falsely proposes v8.30.0) | **keep v8.30.1** — autoupdate downgrade rejected |
| `compilerla/conventional-pre-commit/conventional-pre-commit` | v4.4.0 | v4.4.0 | none | keep |
| `local/agent-worktree-drift-guard` | script | n/a | script exists + executable | keep |
| `local/check-copyright` | script | n/a | script exists + executable | keep |
| `local/check-adr-numbering` | script | n/a | script exists + executable | keep |
| `local/assertion-density` | script | n/a | script exists + executable | keep |
| `local/mypy-local` | `language: system` | `mypy` on PATH | binary present | keep |
| `local/semgrep-local` | `language: python` (additional_deps `semgrep>=1.78,<2.0`) | semgrep on PATH | binary present; **pre-existing io_uring fan-out failure** | **defer to PR #340 (ADR-0867)** |
| `local/ffmpeg-patches-apply-check` | script | n/a | script exists + executable | keep |
| `local/validate-pr-body` | script | n/a | script exists + executable | keep |

## 3. Why each bump or non-bump

### isort 5.13.2 → 6.0.1 (chosen over 9.0.0a3 / 8.0.1)

- `9.0.0a3` is an alpha pre-release; project hygiene pins stable
  releases only.
- 8.0.1 is the latest stable but is two majors away with multiple
  `profile = "black"` interaction changes in 7.x → 8.x; auditing
  those is a separate PR (and a separate ADR).
- 6.0.1 has been the de facto stable line since ~2024 H2, brings
  Python 3.13 / 3.14 parsing fixes the fork needs (we're on Py
  3.14 per ADR-0691 era container pins), and surfaces one latent
  import-grouping fix in
  `tools/vmaf-tune/tests/test_codec_adapter_av1_videotoolbox.py`
  that isort 5.13.2 was silently ignoring.

### ruff-pre-commit v0.15.13 → v0.15.15

- Patch-level bump; no rule selection changes that affect the
  fork's `[tool.ruff.lint]` set.
- `pre-commit run --all-files` confirms zero new findings.

### gitleaks: keep v8.30.1 (reject autoupdate's v8.30.0)

- `git ls-remote --tags --refs https://github.com/gitleaks/gitleaks`
  shows v8.30.1 ranks above v8.30.0 lexicographically and
  chronologically — v8.30.1 is the latest tag.
- pre-commit's autoupdate tag-sort heuristic mis-handles point
  releases that land out of the default branch's commit order;
  documented upstream as a long-standing wart.

### forbid-new-submodules (new)

- Cost: ~0 ms at commit time (no files match in the default tree).
- Benefit: catches a `git submodule add` before it lands, which
  would bypass:
  - `subprojects/*.wrap` pinning (Meson wraps with sha256s),
  - the SBOM machinery (CycloneDX scan walks wraps, not
    `.gitmodules`),
  - and the licence-allow-list audit.
- Fits between `detect-private-key` and the next repo block, with
  an inline comment explaining the dependency posture.

## 4. PR-conflict avoidance

- **PR #340** (`fix(ci): semgrep-local pre-commit hook serial
  execution`, ADR-0867) edits the `semgrep-local` local hook to
  add `require_serial: true`. This audit does NOT touch that
  block.
- **PR #342** (`chore(lint): wire markdownlint-cli2 into make
  lint + pre-commit + CI`, ADR-0866) inserts a new
  `markdownlint-cli2` repo block between `shellcheck-py` and
  `gitleaks`. This audit does NOT touch that file region nor
  insert any block in that range.

## 5. Verification — `pre-commit run --all-files` (post-edit)

```text
trim trailing whitespace........................................................Passed
fix end of files................................................................Passed
check for merge conflicts.......................................................Passed
check yaml......................................................................Passed
check json......................................................................Passed
check toml......................................................................Passed
check for added large files.....................................................Passed
mixed line ending...............................................................Passed
detect private key..............................................................Passed
forbid new submodules.......................................(no files to check)Skipped
clang-format....................................................................Passed
black...........................................................................Passed
isort...........................................................................Passed
ruff check......................................................................Passed
shfmt...........................................................................Passed
shellcheck......................................................................Passed
Detect hardcoded secrets........................................................Passed
ADR-0332 agent worktree-drift guard.............................................Passed
ADR-0105 copyright header present (C/C++/CUDA)..................................Passed
ADR-0386 ADR number collision + heading consistency check.......................Passed
semgrep (.semgrep.yml — local rules, error-gated)..............................Skipped (SKIP=semgrep-local; PR #340 issue)
```

All hooks green except `semgrep-local`, which fails identically on
master tip (io_uring/RLIMIT_MEMLOCK exhaustion under pre-commit's
per-CPU fan-out — fixed by PR #340 / ADR-0867, not by this audit).
A direct `semgrep scan --config=.semgrep.yml --error` against the
worktree exits 0 with 0 findings, confirming the rules themselves
are clean and the failure is purely the pre-commit fan-out shape.

## 6. Reproducer

```bash
git worktree add -b chore/pre-commit-audit-repro /tmp/wt-pre-audit origin/master
cd /tmp/wt-pre-audit

# Show current pinned revs
grep -E '^\s+(repo|rev):' .pre-commit-config.yaml

# Show autoupdate's (sometimes wrong) suggestions
pre-commit autoupdate

# Verify gitleaks tag order manually
git ls-remote --tags --refs https://github.com/gitleaks/gitleaks \
  | grep -E 'v8\.30\.' | sort -V

# Apply the audit deltas (the three rev bumps + forbid-new-submodules)
# … then …
pre-commit run --all-files
# Expect every hook PASS or SKIP except semgrep-local
# (pre-existing io_uring bug fixed by PR #340).
```

## 7. Rebase impact

None against upstream Netflix — `.pre-commit-config.yaml` is
fork-only. The two coordination points are in-flight fork PRs
(#340, #342) and are handled by NOT touching their file regions.

## 8. Follow-ups

- Re-audit `isort` 7.x → 8.x bump as a separate PR once #340 and
  #342 land (separate diff-with-blame on the changed default rules).
- Consider adding `pyupgrade` and `add-trailing-comma` to the
  hygiene chain (deferred — would need an ADR for the
  default-arg-style change those rules enforce across the Python
  tree).
