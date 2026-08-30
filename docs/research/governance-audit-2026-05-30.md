<!-- markdownlint-disable MD013 MD018 MD060 -->
# Research digest: governance file audit — 2026-05-30

ADR: [ADR-0901](../adr/0901-governance-audit.md).
PR: `chore/governance-audit` (this PR).

## 1. Scope of the audit

Inventory the eight conventional community-health and project-
governance artifacts and classify each as present-and-current,
present-but-stale, or missing.

| File                                | Pre-audit state               | Action                          |
|-------------------------------------|-------------------------------|---------------------------------|
| `.github/CODEOWNERS`                | Present; missing fork subtrees| Added 13 new owner rows         |
| `CONTRIBUTING.md`                   | Present; missing 4 sections   | Added branch naming, ADR-0108, ADR allocator, governance pointer |
| `SECURITY.md`                       | Present; current              | No change                       |
| `CODE_OF_CONDUCT.md`                | Present; Contributor Covenant v2.1 | No change                  |
| `.github/PULL_REQUEST_TEMPLATE.md`  | Present; ADR-0108 checklist already in place | No change           |
| `.github/ISSUE_TEMPLATE/*.yml`      | Present (bug, feature, perf, config) | No change                |
| `GOVERNANCE.md`                     | Missing                       | Added                           |
| `MAINTAINERS.md`                    | Missing                       | Added                           |

## 2. Why the additions matter

GitHub's community-health UI surfaces a checklist of these eight
files on every repo. Two were red (`GOVERNANCE.md`,
`MAINTAINERS.md`). Beyond the cosmetic signal, a missing
`GOVERNANCE.md` means an external contributor or a prospective
maintainer has no documented answer for:

- Who decides what?
- How are decisions captured (ADRs vs. issues vs. discussions)?
- How do you become a maintainer?

And a missing `MAINTAINERS.md` means the bus-factor surprise hits
at the worst possible time (a security advisory, a long-lived
contributor wanting to share load) instead of being on the table.

## 3. CODEOWNERS gap analysis

The pre-audit CODEOWNERS file covered:

- The upstream-mirror C subtrees under `/libvmaf/...` (in-flight
  rename to `/core/...` per ADR-0700, being done in PR #321).
- Public C API headers and CLI under `/libvmaf/include/`,
  `/libvmaf/tools/`.
- Build system root files.
- CI / supply chain (`.github/workflows/`, `.github/dependabot.yml`,
  `release-please-config.json`, `.release-please-manifest.json`).
- The Claude agent surface (`.claude/`, `CLAUDE.md`, `AGENTS.md`).
- Netflix golden-data test files (with a "never modify" marker).

Subtrees added since the original CODEOWNERS authoring that were
NOT covered until this PR:

| Subtree            | Phase / ADR  | Why it needs an owner row                  |
|--------------------|--------------|--------------------------------------------|
| `/ai/`             | Phase 3k     | Tiny-AI model training; touches DNN gate   |
| `/core/src/dnn/`   | Phase 3k     | ONNX Runtime integration                   |
| `/model/`          | always       | Shipped VMAF models                        |
| `/mcp-server/`     | Phase 3      | JSON-RPC tool surface                      |
| `/cmd/`            | Phase 4b     | Controller / node / operator / server / tune (ADR-0709) |
| `/deploy/`         | Phase 3      | Helm chart (ADR-0699)                      |
| `/docker/`         | Phase 3      | Production Dockerfile (ADR-0698)           |
| `/dev/`            | Phase 3      | Dev container (`vmaf-dev-mcp`)             |
| `/compat/`         | ADR-0700     | Post-rename Python harness location        |
| `/ffmpeg-patches/` | ADR-0186     | FFmpeg integration patches against n8.1    |
| `/scripts/`        | always       | Build / release / ADR scripts              |
| `/tools/`          | always       | Repository tooling                         |
| `/docs/adr/`       | always       | ADRs are the fork's decision audit trail   |
| `/changelog.d/`    | ADR-0221     | Changelog fragment tree                    |

The catch-all `* @Lusoris` rule already routed reviews for all of
these — the per-subtree rows are about giving each subtree a named
slot so future maintainer additions are mechanical (replace
`@Lusoris` with the new handle on the relevant row).

## 4. Coordination with PR #321

PR #321 (`fix(config): update IDE and lint configs for ADR-0700
libvmaf/ → core/ rename`) rewrites every existing `/libvmaf/...`
row in CODEOWNERS to its `/core/...` equivalent. This PR is
**append-only** for CODEOWNERS — every new row sits below the
existing rows, so there is no line-level conflict with #321. If
#321 lands first, this PR's additions apply cleanly on top. If
this PR lands first, #321 rebases by re-running its rename pass
against the new file and the additions ride along unchanged.

Verified by reading `gh pr view 321 --json files` before drafting:
PR #321's diff for `.github/CODEOWNERS` is +12 / -12, all
rename-only on the existing rows. No new rows are added by #321.

## 5. CONTRIBUTING.md gap analysis

The pre-audit file covered: quickstart, core rules (Conventional
Commits, principles, golden-data gate, cross-backend correctness,
license headers, no force-push), reporting bugs / requesting
features, review expectations, upstream sync, and the inherited
Netflix upstream algorithmic contribution guide.

Gaps surfaced by reading a first-time contributor through the
file:

1. **Branch naming** — Conventional Commits is mentioned for
   commit messages but the branch-prefix convention
   (`feat/<slug>`, `fix/<slug>`, ...) is undocumented. New
   contributors guess.
2. **ADR-0108 deliverables** — the file does not mention the
   six-deliverables gate; the contributor only sees it when the
   PR is rejected by the CI parser.
3. **ADR allocator** — `scripts/adr/next-free.sh --claim` is
   documented in `CLAUDE.md` §12 r8 and in ADR-0535 /
   ADR-0628, but not in the file external contributors actually
   read.
4. **Governance** — no pointer to `GOVERNANCE.md` or
   `MAINTAINERS.md`.

All four gaps are now closed in the same section block.

## 6. What this audit did NOT change

- `SECURITY.md` — content was already comprehensive: supported
  versions, private disclosure channel, alternative email + PGP
  request, response timeline, supply-chain guarantees (SBOM,
  Sigstore, SLSA L3), known non-vulnerabilities. No drift.
- `CODE_OF_CONDUCT.md` — Contributor Covenant v2.1 with the
  enforcement email pointing at `lusoris@pm.me`. No drift.
- `.github/PULL_REQUEST_TEMPLATE.md` — already carries the
  ADR-0108 checklist with the strict sentinel forms required by
  the parser. No drift.
- `.github/ISSUE_TEMPLATE/{bug_report,feature_request,performance_regression,config}.yml` —
  bug template covers backend selection (CPU/AVX2/AVX-512/NEON/
  CUDA/SYCL/HIP), version+OS+hardware, logs, and the Netflix
  golden checkbox. Feature template covers scope dropdown.
  Performance template requires bisect output, before/after
  numbers, hardware. Config file routes security to private
  flow and points at upstream Netflix for upstream-only issues.
  No drift.
- The existing CODEOWNERS rows (still using `/libvmaf/...`) —
  deferred to PR #321 to avoid merge-conflict on rename.

## 7. Reproducer

```bash
# Before this PR — community-health check finds missing files:
test -f GOVERNANCE.md || echo "MISSING: GOVERNANCE.md"
test -f MAINTAINERS.md || echo "MISSING: MAINTAINERS.md"
grep -q "Deep-dive deliverables" CONTRIBUTING.md || echo "MISSING: ADR-0108 in CONTRIBUTING.md"
grep -q "^/ai/" .github/CODEOWNERS || echo "MISSING: /ai/ owner row"

# After this PR — all four checks silent.
```

## References

- ADR-0901 (this PR's ADR).
- Concurrent: PR #321 — CODEOWNERS path rewrite for ADR-0700.
- ADR-0108 — Deep-dive deliverables.
- ADR-0700 — VMAFX repo layout.
