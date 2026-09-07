<!-- markdownlint-disable MD013 MD060 -->
# ADR-1227: Workflow display names are short labels; the axis list lives in the file

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: ci, docs, fork-local

## Context

[ADR-0116](0116-ci-workflow-naming-convention.md) §2 set the workflow-level
`name:` convention as "Title Case, with an em-dash separating area from
scope/axis list", giving names like:

```text
Tests & Quality Gates — Netflix Golden / Sanitizers / Tiny AI / Coverage   (72)
Security Scans — Semgrep / CodeQL / Gitleaks / Dependency Review           (64)
libvmaf Build Matrix — Linux/macOS/Windows/ARM × CPU/SYCL/CUDA             (62)
FFmpeg Integration — Linux/macOS × gcc/clang + SYCL                        (51)
Upstream watcher — FFmpeg HIP hwdec (closes T-FFMPEG-HIP-FILTER-DEFERRED)  (73)
```

That reads well in a YAML file. It renders badly everywhere the name is
actually consumed. GitHub's `badge.svg` endpoint paints the **workflow name**
into the badge, so the seven status badges at the top of
[`README.md`](../../README.md) — whose link labels are already the short
`Tests` / `Security` / `Builds` / `FFmpeg` — render as 60-to-70-character
banners that wrap the README header onto several lines. The same names crowd
the Actions sidebar and `gh run list` output.

The irony is that ADR-0116's sibling rule already fixed this one level down:
`docs/development/ci-job-names.md` sets a ≤30-character budget for *job*
display names, with the reasoning that verbose parentheticals and citations
belong in workflow comments rather than the check name. Workflow names were
simply never brought under the same rule.

## Decision

We will apply the existing ≤30-character job-name budget to workflow-level
`name:` fields as well, superseding [ADR-0116](0116-ci-workflow-naming-convention.md)
§2 (workflow `name:`) only. Every other clause of ADR-0116 stands: filenames
stay purpose-descriptive kebab-case, job names keep their Title Case axis tags,
and status-check contexts keep deriving from job names.

The axis list the old convention carried in the name moves to a comment at the
top of the workflow file, where it does not have to fit in a badge.

**Filenames do not change.** The badge and Actions URLs are keyed on the
filename, so this is a label-only edit — no README badge URL churn, no
branch-protection re-pin, and no re-pointing of any `workflow_run` trigger
(none of the renamed workflows is referenced by name).

Applied:

| File | Before | After |
| --- | --- | --- |
| `tests-and-quality-gates.yml` | 72 chars | `Tests` |
| `security-scans.yml` | 64 chars | `Security` |
| `libvmaf-build-matrix.yml` | 62 chars | `Builds` |
| `ffmpeg-integration.yml` | 51 chars | `FFmpeg` |
| `e2e-k8s.yml` | 45 chars | `E2E` |
| `dev-container-build.yml` | 29 chars | `Dev Container` |
| `dev-container-publish.yml` | 21 chars | `Dev Container Publish` |
| `docker-publish-production.yml` | 25 chars | `Publish Production` |
| `docker-publish-operator-node.yml` | 28 chars | `Publish Operator Node` |
| `fuzz.yml` | 24 chars | `Fuzz` |
| `sycl-parity.yml` | 24 chars | `SYCL Parity` |
| `upstream-ffmpeg-hip-hwdec-watcher.yml` | 73 chars | `Watcher — FFmpeg HIP hwdec` |
| `upstream-netflix-955-watcher.yml` | 61 chars | `Watcher — Netflix#1494` |
| `upstream-netflix-645-hdr-model-watcher.yml` | 50 chars | `Watcher — Netflix HDR model` |

The four names the README badges render are deliberately made identical to the
badge link labels that were already there, so the badge text and the link text
finally agree.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Leave the names and drop the badges from the README | No CI churn | The badges are the fastest signal of whether master is green; removing a user-facing surface to avoid renaming a label is backwards | Rejected |
| Shorten only the four workflows the README badges point at | Minimal diff | Leaves the same defect in the Actions sidebar for the other ten, and the next badge added re-opens it | Rejected — fix the rule, not the four instances |
| Use `shields.io` static badges with hand-written labels | Full control of badge text | Static badges do not show live status, which is the entire point of these seven | Rejected |
| Keep the axis list but abbreviate it (`Tests — Golden/ASan/AI/Cov`) | Retains some axis information | Still 26+ chars of noise in a badge whose job is to say pass/fail, and the abbreviations need their own glossary | Rejected — the axis list belongs in the file |

## Consequences

- **Positive**: the README header renders on one line again; the Actions
  sidebar and `gh run list` become scannable; badge text and link label agree.
- **Negative**: a workflow's axis coverage is no longer visible from its name
  alone — a reader has to open the file (or `docs/development/ci-job-names.md`)
  to see which legs it runs. The header comment carries it.
- **Neutral / follow-ups**: `docs/development/ci-job-names.md` gains a
  workflow-name section so the two budgets are documented together. Historical
  `CHANGELOG.md` and `changelog.d/` entries quoting the old names are left
  alone: they are an accurate record of what the names were at the time.

## References

- req: the user asked for the CI names to be shortened so the badges render
  correctly again.
- [ADR-0116](0116-ci-workflow-naming-convention.md) — superseded on the
  workflow `name:` clause only.
- [`docs/development/ci-job-names.md`](../development/ci-job-names.md) — the
  ≤30-character job-name budget this extends.
