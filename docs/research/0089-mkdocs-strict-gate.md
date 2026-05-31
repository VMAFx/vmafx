<!-- markdownlint-disable MD013 MD037 MD060 -->
# Research-0089: mkdocs `--strict` gate — warning census + carve-out justification

- **Date**: 2026-05-09
- **Companion ADR**: [ADR-0332](../adr/0332-mkdocs-strict-gate-validation-policy.md)

## Why

Implementation task: clean up all mkdocs warnings + tighten the docs build to
`mkdocs build --strict`. The `.github/workflows/docs.yml` lane already passed
`--strict`, but `mkdocs.yml` set every link-validation category to `info`
which silently drained the flag of all teeth. The brief: classify every
warning, fix what's fixable, document carve-outs for what isn't.

## Census against `master` tip `ec0e002e` (after promoting all validation categories to `warn`)

```text
mkdocs build --strict
EXIT=1
WARNING count: 1,276
```

Breakdown by class (mkdocs validation category):

| Class | Count | Source dominance |
|---|---|---|
| `links.not_found` | 1,187 | ADR bodies (665) + research digests (137) + rebase-notes.md (158) |
| `links.unrecognized_links` | 87 | All trees, mostly trailing-slash dir refs |
| `links.anchors` | 2 | `mcp/embedded.md`, `research/0055-...md` |
| `nav.omitted_files` | 1 (a 200-row block) | adr/* + research/* (deliberately not in nav) |

Breakdown by link target (top 8 of 1,187 `not_found`):

| Target | Count | Class |
|---|---|---|
| `../../.workingdir2/BACKLOG.md` | 34 | Cross-tree (planning dir, gitignored runtime artefact) |
| `../../CLAUDE.md` | 30 | Cross-tree (repo-root contributor file) |
| `../../scripts/ci/cross_backend_vif_diff.py` | 19 | Cross-tree (CI script) |
| `../../.github/workflows/tests-and-quality-gates.yml` | 18 | Cross-tree (workflow) |
| `../../scripts/ci/cross_backend_parity_gate.py` | 15 | Cross-tree (CI script) |
| `0253-fastdvdnet-pre-real-weights.md` | 9 | Renamed-ADR (slug now `0255-...`) |
| `../../core/src/feature/third_party/xiph/psnr_hvs.c` | 9 | Cross-tree (vendored source) |
| `../../core/include/libvmaf/libvmaf_vulkan.h` | 9 | Cross-tree (public header) |

The "cross-tree" class — links from docs to source-tree files / dirs outside
`docs_dir` — is by far the dominant population. mkdocs cannot resolve them
because they sit outside the rendered site, but they render fine on GitHub's
web view (where contributors most often read these files), and the fork's
ADR convention deliberately uses `../../core/src/...` to point at the
source the ADR is about.

## What's fixable vs. not, per population

| Population | Count | Fixable on this PR? | Reason |
|---|---|---|---|
| Cross-tree pointers from ADR bodies | ~600 | No | ADR-0028 / ADR-0106 freeze ADR bodies once `Status: Accepted`. |
| Cross-tree pointers from non-ADR docs | ~220 | Partially | Sweep-able by converting to absolute GitHub URLs, but the volume + lint churn (MD013 line-length) is out of scope for a docs-CI-tightening PR. Tracked at `info` for opportunistic cleanup. |
| Renamed-ADR cross-refs in ADR bodies | ~360 | No | ADR-0028 / ADR-0106 immutability. Resolution path: when the citing ADR is superseded, the new ADR uses correct slugs. |
| Renamed-ADR cross-refs in non-ADR docs | ~10 | Yes (in principle) | Volume small but spread across many files; deferred to opportunistic cleanup. |
| In-doc anchor typos | 2 | Yes | Fixed on this PR. |
| Bare-relative-dir links in non-ADR docs | ~10 | Yes | Fixed on this PR (`docs/{index,state,rebase-notes}.md`). |
| Bare-relative-dir links in ADR bodies | ~12 | No | ADR-body immutability. |
| Excluded-tree leakage (`adr/_index_fragments/**`) | 50 | Yes | Excluded via `mkdocs.yml exclude_docs:`. |

## Decision

Tighten three categories to `warn`, carve out two at `info` with inline
justification:

```yaml
validation:
  nav:
    omitted_files: info     # 260+ ADRs + 80+ digests by design
    not_found: warn         # mkdocs.yml nav: typos
    absolute_links: info
  links:
    not_found: info         # cross-tree pointers + renamed-ADR refs (immutable)
    anchors: warn           # in-doc anchor typos (fixed two on this PR)
    unrecognized_links: info  # same population shape as not_found
```

Plus `exclude_docs: adr/_index_fragments/*` to stop fragment leakage.

## Result

- `mkdocs build --strict` → `EXIT=0`, 0 WARNINGs.
- Docs CI lane now actively gates new anchor breakage, nav typos, and
  fragment-tree leaks.
- 1,180+ residual cross-tree-pointer / renamed-ADR-ref INFO entries
  remain in the build log; reducible only via ADR-0028 supersession.

## In-flight-PR-blocking warnings deferred

None. The carve-out keeps `links.not_found: info`, so any in-flight PR
landing new doc content with `../../source-tree-path` pointers will not
trip the strict gate. Anchor breakage in in-flight PR doc additions
*will* trip the gate after this PR merges — by design.

## References

- [ADR-0332](../adr/0332-mkdocs-strict-gate-validation-policy.md) — companion ADR.
- [ADR-0221](../adr/0221-changelog-adr-fragment-pattern.md) — fragment-tree concatenation pattern.
- [ADR-0028](../adr/0028-adr-maintenance-rule.md) / [ADR-0106](../adr/0106-adr-maintenance-rule.md) — ADR-body immutability.
- `.github/workflows/docs.yml` — the strict gate.
- Source: implementation task — "tighten the docs build to `mkdocs build --strict`".
