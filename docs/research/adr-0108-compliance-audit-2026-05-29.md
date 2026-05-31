<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0108 Six-Deliverables Compliance Audit — 2026-05-29

**Scope:** Five most recently merged PRs as of 2026-05-29
**Gate:** ADR-0108 six deliverables (see `docs/adr/0108-deep-dive-deliverables-rule.md`)
**Auditor:** automated agent (Claude Sonnet 4.6)

---

## 1. PRs sampled

| # | PR | Title | Merged |
|---|---|---|---|
| 1 | #1573 | fix(hip): wave32 carry-preserving int64 reduction in VIF and motion | 2026-05-28T09:12Z |
| 2 | #1568 | chore(build): bump C standard to C23 | 2026-05-28T09:47Z |
| 3 | #1582 | feat(cli): --netflix-compat flag (replaces inverted #1569) | 2026-05-28T09:48Z |
| 4 | #1583 | feat(server): vmafx-server HTTP transport + observability foundation | 2026-05-28T09:51Z |
| 5 | #1571 | refactor(meta): VMAFX repo layout — libvmaf/ → core/ | 2026-05-28T10:09Z |

---

## 2. Deliverable matrix

Legend: ✓ = present and substantive | ✗ = absent or only opt-out sentinel missing

| Deliverable | #1573 | #1568 | #1582 | #1583 | #1571 |
|---|:---:|:---:|:---:|:---:|:---:|
| D1 Research digest under `docs/research/` | ✓ | ✓ | ✓ | ✓ | ✓ |
| D2 Decision matrix in ADR `## Alternatives considered` | ✓ | ✓ | ✓ | ✓ | ✓ |
| D3 `AGENTS.md` invariant note | ✓ | ✓ | ✓ | ✗ | ✗ |
| D4 Reproducer / smoke-test command in PR body | ✓ | ✓ | ✓ | ✓ | ✓ |
| D5 `changelog.d/<section>/<topic>.md` fragment | ✓ | ✓ | ✓ | ✓ | ✓ |
| D6 `docs/rebase-notes.md` entry | ✓ | ✓ | ✓ | ✓ | ✓ |

**Score: 28 / 30 = 93 %**

---

## 3. Findings per PR

### PR #1573 — fix(hip): wave32 carry-preserving int64 reduction

**All 6 deliverables present.**

- D1: `docs/research/0688-hip-raphael-igpu-divergence.md` (185 lines, root-cause analysis of RDNA2 wave32 + int64 carry loss)
- D2: ADR-0688 `## Alternatives considered` table — 4 options (runtime warpSize, LLVM PTX shim, CUDA-style warpSize helper, no-fix)
- D3: PR body opts out with sentinel "no rebase-sensitive invariants beyond what is in `docs/rebase-notes.md`"; `docs/rebase-notes.md` entry confirms the HIP files are fork-local with no upstream equivalent — **valid opt-out**
- D4: Docker `vmaf-dev-mcp` reproducer with `HSA_OVERRIDE_GFX_VERSION` and JSON score assertion
- D5: `changelog.d/fixed/hip-wave32-vif-motion-carry-loss.md`
- D6: `docs/rebase-notes.md` `fix/hip-wave32-vif-motion-20260528` section

### PR #1568 — chore(build): bump C standard to C23

**All 6 deliverables present.**

- D1: No digest sentinel ("no research digest needed: trivial build-standard bump") — valid per ADR-0108 "trivial" exemption
- D2: ADR-0692 `## Alternatives considered` — 3 options (C11 indefinitely, C17 first, keep c2x alias)
- D3: Opt-out sentinel "no rebase-sensitive AGENTS.md invariants beyond what is documented in `docs/rebase-notes.md`"; `docs/rebase-notes.md` carries the C23 empty-parameter-list hazard — **valid opt-out**
- D4: `meson setup /tmp/build-c23 libvmaf -Denable_cuda=false ...` with expected output
- D5: `changelog.d/changed/vmafx-c23-bump.md`
- D6: `docs/rebase-notes.md` `chore/vmafx-c23-bump` section with upstream divergence note

### PR #1582 — feat(cli): --netflix-compat flag

**All 6 deliverables present.**

- D1: No digest sentinel ("trivial CLI flag addition") — valid
- D2: ADR-0696 `## Alternatives considered` — 4 options (precision alias, legacy binary, env var, no escape hatch)
- D3: `libvmaf/tools/AGENTS.md` updated with ordering invariant: `netflix_compat` block must run after `vmafx_mode` block
- D4: `vmafx --netflix-compat --version` and scored-YUV test in PR body
- D5: `changelog.d/added/vmafx-netflix-compat.md`
- D6: `docs/rebase-notes.md` entry

### PR #1583 — feat(server): vmafx-server HTTP transport

**D3 missing — no AGENTS.md change and no opt-out sentinel in PR body.**

- D1: No digest sentinel ("foundation scaffold, ADR-0701 captures design decisions") — valid
- D2: ADR-0701 `## Alternatives considered` — 4 options (Go+gRPC, FastAPI, MCP-over-HTTP, sidecar)
- D3: **GAP.** PR body says "no rebase-sensitive invariants" but provides no formal sentinel text per ADR-0108 ("no rebase-sensitive invariants" must appear verbatim as the opt-out). More importantly, the `mcp-server/vmaf-mcp/` subtree gained a new dependency group (`[http]` extra in `pyproject.toml`) that a future rebase agent should know about. The opt-out is defensible but undocumented in any `AGENTS.md` under `mcp-server/`
- D4: `vmaf-mcp --transport http --port 8080 & ; curl localhost:8080/healthz` → `{"status":"healthy"}`
- D5: `changelog.d/added/vmafx-server-foundation.md`
- D6: `docs/rebase-notes.md` `feat/vmafx-server-foundation-20260528` section

### PR #1571 — refactor(meta): VMAFX repo layout libvmaf/ → core/

**D3 missing — AGENTS.md changes are path-reference updates, not a new invariant note.**

- D1: No digest sentinel ("trivial directory rename, no algorithmic change") — valid
- D2: ADR-0700 `## Alternatives considered` — 4 options (keep unchanged, rename libvmaf/ only, rename module to python_vmaf, vmafx-core/)
- D3: **GAP.** The PR updated every `AGENTS.md` file in the tree (all path references from `libvmaf/` → `core/`), but the actual invariant that matters for rebases — "any patch or rebase touching `libvmaf/` paths must be adapted to `core/`" — is documented in `docs/rebase-notes.md` (the `refactor/meta/vmafx-repo-layout` recipe) but was NOT added to `AGENTS.md §Rebase-sensitive invariants`. The root `AGENTS.md` §13 index was not updated with a row for this rename.
- D4: Build + Python import smoke test in PR body
- D5: `changelog.d/changed/vmafx-repo-layout.md`
- D6: `docs/rebase-notes.md` carries the full upstream-sync recipe

---

## 4. Recurring gaps

**D3 (AGENTS.md invariant note)** is the only deliverable with consistent failures (2 / 5 PRs).

Both failures are on the "borderline opt-out" pattern: the PR author treats `docs/rebase-notes.md` as a substitute for the formal `AGENTS.md §Rebase-sensitive invariants` index row without registering the gap explicitly. The two cases differ in severity:

| PR | Gap severity | Impact |
|---|---|---|
| #1583 (server) | Low — genuinely no C-source invariants; Python dep change is low rebase risk | A future rebase agent scanning `mcp-server/AGENTS.md` will not see the `[http]` pyproject extra |
| #1571 (layout rename) | Medium — the `libvmaf/` → `core/` rename is the highest-rebase-risk change in the set; every in-flight downstream branch is affected | Root `AGENTS.md` §13 index is missing the rename row |

**Root cause:** ADR-0108 requires the opt-out sentinel to appear as a checklist item; both PRs describe the opt-out in prose in the body but do not tick the `[x] AGENTS.md invariant: no rebase-sensitive invariants` checkbox per the template format, making the opt-out harder to parse by automation.

---

## 5. Improvement plan

1. **Immediate fix (this PR):** Add the missing `AGENTS.md §Rebase-sensitive invariants` index row for the `libvmaf/ → core/` rename (PR #1571 gap). The `mcp-server/vmaf-mcp/AGENTS.md` entry for the `[http]` dep group is low-priority but included for completeness.

2. **Process fix:** Update the PR template checklist so D3 has an explicit "`[x] AGENTS.md invariant: <note> / no rebase-sensitive invariants: <reason>`" placeholder that forces the author to choose one branch — prose in the body will not satisfy the automated audit.

3. **Automation hook (future):** A pre-merge CI script can grep for the D3 sentinel in the PR body and flag PRs where `AGENTS.md` is not touched and the sentinel is absent.

---

## 6. References

- ADR-0108: `docs/adr/0108-deep-dive-deliverables-rule.md`
- ADR-0688: `docs/adr/0688-hip-wave32-vif-motion-fix.md`
- ADR-0692: `docs/adr/0692-vmafx-c23-bump.md`
- ADR-0696: `docs/adr/0696-vmafx-netflix-compat.md`
- ADR-0700: `docs/adr/0700-vmafx-repo-layout.md`
- ADR-0701: `docs/adr/0701-vmafx-cloud-native-redesign.md`
