<!-- markdownlint-disable MD013 -->
# ADR-0166: MCP server release artifact channel — PyPI + GitHub release attachment + Sigstore (T7-2)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: release, mcp, supply-chain, sigstore, pypi

## Context

The fork ships an MCP (Model Context Protocol) server at
[`mcp-server/vmaf-mcp/`](../../mcp-server/vmaf-mcp/) that exposes
VMAF scoring, model listing, and benchmark runs over JSON-RPC. The
server is a Python package (`vmaf-mcp`, `pyproject.toml` declares
`>=3.10`, optional `eval` extras pull in ONNX / pandas / scipy).

Today the package has no release channel:

- No `pypi` upload — `pip install vmaf-mcp` returns "no matching
  distribution".
- No GitHub release attachment — release tags publish
  `libvmaf.so` + `vmaf` CLI + `models.tar.gz` (see
  [`supply-chain.yml`](../../.github/workflows/supply-chain.yml))
  but skip the MCP server.
- No signature, no provenance, no SBOM for the MCP layer.

[BACKLOG T7-2](../../.workingdir2/BACKLOG.md) flagged the gap as
"PyPI vs GitHub release attachment vs both" — pending an ADR plus
release.yml wiring.

## Decision

**Ship both channels with full Sigstore + SLSA provenance.**

1. **PyPI** via Trusted Publishing (OIDC, no token in CI).
   - Distribution name: `vmaf-mcp`
   - Trigger: `release: published` events for any tag matching
     `v*-lusoris.*` (the fork's release-please scheme).
   - Builds wheel + sdist via `python -m build`.
   - Publishes via `pypa/gh-action-pypi-publish` with attestations
     (PEP 740 — `--attestations` flag emits Sigstore-signed
     provenance the index stores alongside the artifact).
2. **GitHub release attachment** alongside the existing libvmaf
   artifacts.
   - Same wheel + sdist as PyPI.
   - Cosign keyless `.bundle` (matches the libvmaf pattern in
     `supply-chain.yml`).
   - SBOM (SPDX + CycloneDX) covering the Python dependency tree.
   - SLSA L3 provenance — wheel + sdist hashes feed the existing
     `slsa-github-generator` reusable workflow alongside the
     libvmaf artifacts.

Both channels are wired in the **existing** `supply-chain.yml`
workflow under a new `mcp-build` + `mcp-publish` job pair, not a
separate workflow file. Reusing the workflow keeps the per-release
matrix (build → SBOM → sign → SLSA → attach) coherent across
libvmaf + MCP.

User-facing install paths after this ships:

```bash
# Recommended: PyPI
pip install vmaf-mcp

# Pinnable to a libvmaf release (signature-verifiable)
gh release download v3.x.y-lusoris.N --pattern 'vmaf_mcp-*.whl'
cosign verify-blob \
  --bundle vmaf_mcp-X.Y.Z-py3-none-any.whl.bundle \
  vmaf_mcp-X.Y.Z-py3-none-any.whl
pip install vmaf_mcp-X.Y.Z-py3-none-any.whl
```

## Alternatives considered

1. **GitHub release attachment only.** Avoids PyPI namespace
   reservation + the maintainer burden of a Trusted Publishing
   configuration. Rejected because it leaves `pip install vmaf-mcp`
   broken, which is the discovery path most MCP integrators expect.
2. **PyPI only.** Loses the "pin to a libvmaf release" guarantee:
   PyPI versions advance independently from libvmaf release tags
   unless we couple them, which adds complexity. The GitHub
   attachment path is also a no-cost extension of the existing
   `supply-chain.yml` so adding it is essentially free.
3. **Conda-forge / homebrew.** Out of scope for this ADR; can be
   added later. PyPI is the canonical Python channel and matches
   the `pyproject.toml` declaration already shipped.
4. **Token-based PyPI publish (`__token__` + secret).** Rejected
   for OpenSSF Scorecard reasons — Trusted Publishing via OIDC
   eliminates the long-lived secret entirely. The fork's
   `scorecard.yml` already advertises Sigstore-signed dashboard
   attestations; staying OIDC-only across release surfaces is
   consistent.

## Consequences

**Positive:**

- Discovery: `pip install vmaf-mcp` works from day one of the next
  release.
- Provenance: every wheel + sdist carries a Sigstore bundle and
  PEP 740 attestation; consumers can verify offline.
- No long-lived secrets: Trusted Publishing requires no PyPI token
  in repo settings.
- One workflow surface: extending `supply-chain.yml` keeps the
  release matrix coherent.

**Negative:**

- One-time setup: a Trusted Publisher entry must be configured on
  PyPI (organisation `lusoris`, repository `vmaf`, workflow
  `supply-chain.yml`, environment `pypi-publish`). This is a
  user-driven UI step — see "Operational notes" below.
- Initial namespace reservation: until the first release, PyPI's
  `vmaf-mcp` name is unreserved and could be squatted. Reserve by
  uploading the first build manually to PyPI under the
  `lusoris` account before the next release tag, OR rely on
  Trusted Publishing's "first-publish" flow which auto-reserves.
- SBOM growth: covering the Python dependency tree adds ~40 kB of
  metadata per release; insignificant compared to libvmaf.

## Operational notes

One-time setup (user, before the first release after this PR
merges):

1. Log in to <https://pypi.org/manage/account/publishing/> as the
   project owner.
2. Add a Pending Trusted Publisher with:
   - PyPI Project Name: `vmaf-mcp`
   - Owner: `lusoris`
   - Repository: `vmaf`
   - Workflow: `supply-chain.yml`
   - Environment: `pypi-publish`
3. (Optional) Reserve the `vmaf-mcp` name by manually uploading
   `vmaf_mcp-0.0.1.dev0` once before the next tagged release.

The first `release: published` event after step 2 completes the
publisher binding automatically; subsequent releases require no
further user interaction.

## References

- [BACKLOG T7-2](../../.workingdir2/BACKLOG.md) — backlog row.
- [`supply-chain.yml`](../../.github/workflows/supply-chain.yml) —
  existing release workflow being extended.
- [`mcp-server/vmaf-mcp/pyproject.toml`](../../mcp-server/vmaf-mcp/pyproject.toml) —
  package metadata.
- [PEP 740 — Trusted Publishing attestations](https://peps.python.org/pep-0740/)
- [PyPI Trusted Publishers docs](https://docs.pypi.org/trusted-publishers/)
- `req` — user popup choice 2026-04-25: "Both PyPI and GitHub
  release attachment (Recommended)".
