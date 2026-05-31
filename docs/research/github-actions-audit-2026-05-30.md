<!-- markdownlint-disable MD060 -->
# Research digest: GitHub Actions hardening audit (2026-05-30)

Companion research digest for [ADR-0875](../adr/0875-github-actions-audit-2026-05-30.md).

## Scope

Audit every workflow under `.github/workflows/` for three OpenSSF
Scorecard / GitHub-Actions-security properties:

1. Every third-party action pinned to a 40-character commit SHA.
2. Top-level `permissions:` block present and restrictive.
3. `persist-credentials: false` on every `actions/checkout` that does
   not push to git.

## Method

A Python script walked all 24 workflow files and classified each
`uses:` line (tag vs SHA), each top-level `permissions:` block
(present / restrictive / wide), and each `actions/checkout` step
(persist-credentials set within a 15-line window after the `uses:`).

Two workflows (`lint-and-format.yml`, `libvmaf-build-matrix.yml`)
were excluded from edits because they are touched by in-flight PRs
(PR #342 markdownlint wiring, PR #325 dead Vulkan/MoltenVK lane
removal). They will be re-audited after those PRs land.

## Findings

### Pinning

All 153 `uses:` lines across the 22 audited workflows were already
SHA-pinned (40-character hex with trailing `# vX.Y.Z` comment for
human readers). This is the result of the prior Scorecard remediation
campaign recorded under
`changelog.d/security/ossf-scorecard-remediation.md`. No tag-pinned
actions remain.

### Top-level permissions

22 of 24 workflows already declare a top-level `permissions:` block:

| Workflow | Top-level perms |
|---|---|
| `build.yml` | `contents: read` |
| `docker-image.yml` | `contents: read` |
| `docker-publish-production.yml` | `contents: read` |
| `docs.yml` | `contents: read`, `pages: write`, `id-token: write` |
| `ffmpeg-integration.yml` | `contents: read` |
| `fuzz.yml` | `contents: read` |
| `go-ci.yml` | (missing) |
| `nightly-bisect.yml` | `contents: read`, `issues: write` |
| `nightly.yml` | `contents: read` |
| `release-please.yml` | `contents: read` |
| `required-aggregator.yml` | `actions: read`, `contents: read`, `checks: read`, `pull-requests: read` |
| `rule-enforcement.yml` | `contents: read`, `pull-requests: write` |
| `rust-ci.yml` | (missing) |
| `sanitizers.yml` | `contents: read` |
| `scorecard.yml` | `read-all` |
| `security-scans.yml` | `contents: read` |
| `supply-chain.yml` | `contents: read` |
| `tests-and-quality-gates.yml` | `contents: read` |
| `upstream-ffmpeg-hip-hwdec-watcher.yml` | `contents: read`, `issues: write` |
| `upstream-netflix-645-hdr-model-watcher.yml` | `contents: read`, `issues: write` |
| `upstream-netflix-955-watcher.yml` | `contents: read`, `issues: write` |
| `upstream-watcher.yml` | `read-all` |

Action: add `permissions: contents: read` to `go-ci.yml` and
`rust-ci.yml`.

### Per-job permissions

Wide per-job permissions are present where genuinely needed and all
are justifiable:

| Workflow / job | Permission | Justification |
|---|---|---|
| `docker-publish-production.yml::build-*` | `packages: write, id-token: write, attestations: write` | Push image to GHCR + OIDC-sign + attest |
| `release-please.yml::release-please` | `contents: write, pull-requests: write` | Manage release PR |
| `rule-enforcement.yml` (multiple) | `pull-requests: write` | Post checklist comments |
| `scorecard.yml::analysis` | `security-events: write, id-token: write` | Upload SARIF + sign |
| `security-scans.yml` (all) | `security-events: write` | Upload CodeQL/semgrep SARIF |
| `supply-chain.yml::sign,slsa-provenance,mcp-sign,mcp-publish-pypi` | `id-token: write` | OIDC keyless signing |
| `supply-chain.yml::attach-to-release` | `contents: write` | Attach signed artefact to release |

OIDC adoption (`id-token: write`) is already in place for every step
that needs cloud / Sigstore / PyPI auth, replacing long-lived
secrets. No `secrets.<NAME>` outside of `GITHUB_TOKEN` is referenced
anywhere in the workflow tree.

### `persist-credentials: false`

40+ checkout steps already opt out of credential persistence. Five
remained:

| File | Line | Job | Pushes to git? |
|---|---|---|---|
| `sanitizers.yml` | 63 | `asan-ubsan` | No |
| `sanitizers.yml` | 117 | `tsan` | No |
| `supply-chain.yml` | 34 | `build-artifacts` | No (uploads via `actions/upload-artifact`) |
| `supply-chain.yml` | 100 | `sbom` | No (downloads artifact + uploads SBOM file) |
| `supply-chain.yml` | 178 | `mcp-build` | No (builds wheel) |

The `supply-chain.yml` cases were carried under the older blanket
whitelist ("supply-chain.yml legitimately needs the credential") but
actual examination shows only the downstream `sign`,
`slsa-provenance`, `mcp-sign`, `mcp-publish-pypi`, and
`attach-to-release` jobs need a non-default token, and those jobs
have their own checkouts (or use OIDC instead of `GITHUB_TOKEN`).

Action: add `persist-credentials: false` to all five.

### Reusable workflows

`grep workflow_call .github/workflows/` returned zero hits. The fork
has no reusable workflows yet; the "inputs/outputs explicitly
declared" check is vacuously satisfied.

### Cloud-auth OIDC

Already adopted everywhere it applies:

- Sigstore keyless signing (`supply-chain.yml::sign`,
  `supply-chain.yml::mcp-sign`) — `id-token: write`.
- SLSA provenance (`supply-chain.yml::slsa-provenance`) — `id-token: write`.
- PyPI trusted publishing (`supply-chain.yml::mcp-publish-pypi`) — `id-token: write`.
- GitHub Pages deploy (`docs.yml`) — `id-token: write`.
- Scorecard SARIF upload (`scorecard.yml::analysis`) — `id-token: write`.

No long-lived AWS / GCP / Docker Hub / PyPI password secrets remain.

## Outcome

Four files modified:

- `.github/workflows/go-ci.yml` — add top-level `permissions: contents: read`.
- `.github/workflows/rust-ci.yml` — add top-level `permissions: contents: read`.
- `.github/workflows/sanitizers.yml` — add `persist-credentials: false`
  to both checkouts.
- `.github/workflows/supply-chain.yml` — add `persist-credentials: false`
  to the `build-artifacts`, `sbom`, and `mcp-build` checkouts.

All four files parse as valid YAML. The remaining two workflows
(`lint-and-format.yml`, `libvmaf-build-matrix.yml`) carry the same
properties already; they were skipped only to avoid touching files
held by in-flight PRs.

## Reproduce

```bash
python3 <<'PY'
import re, glob
files = sorted(glob.glob('.github/workflows/*.yml'))
SHA_RE = re.compile(r'^[0-9a-f]{40}$')
USES_RE = re.compile(r'^\s*-?\s*uses:\s*([^\s#]+)')
for f in files:
    with open(f) as fh:
        text = fh.read()
    lines = text.splitlines()
    has_top = any(re.match(r'^permissions:', ln) for ln in lines[:60])
    if not has_top:
        print(f'MISSING top-perms: {f}')
    for i, ln in enumerate(lines):
        m = USES_RE.match(ln)
        if m and '@' in m.group(1):
            action, version = m.group(1).rsplit('@', 1)
            if not action.startswith('./') and not SHA_RE.match(version):
                print(f'UNPINNED: {f}:{i+1}  {ln.strip()}')
        if re.match(r'^\s*-?\s*uses:\s*actions/checkout@', ln):
            if not any('persist-credentials' in w for w in lines[i:i+15]):
                print(f'MISSING persist-creds: {f}:{i+1}')
PY
```

After the audit-fix PR, this script should print nothing for the 22
in-scope workflows.
