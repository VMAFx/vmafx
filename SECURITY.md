# Security Policy

## Supported versions

| Version                         | Supported                                                    |
|---------------------------------|--------------------------------------------------------------|
| Latest two VMAFx `3.x` releases | :white_check_mark:                                           |
| Netflix/vmaf `3.x`              | see [Netflix/vmaf](https://github.com/Netflix/vmaf/security) |
| VMAFx `< 3.0`                   | :x:                                                          |

The fork tracks upstream Netflix/vmaf's supported-version policy for the parts
of the code inherited from upstream. Fork-only code paths
(`core/src/{cuda,sycl,hip,metal,mcp}/`,
`core/src/feature/{cuda,sycl,hip,metal}/`, GPU backend runtimes, Tiny-AI /
ONNX Runtime surface, embedded + standalone MCP servers) are supported on
the current `master` and the latest two tagged releases.

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Use GitHub's private vulnerability-reporting flow:
`https://github.com/VMAFx/vmafx/security/advisories/new`

If the issue is in code inherited from upstream, we will coordinate
disclosure with Netflix/vmaf maintainers.

Alternative channels:

- Email: `lusoris@pm.me` — PGP-encrypt anything sensitive; request the
  public key via the same address.

Please include:

1. Affected version(s) / commit SHA.
2. A minimal reproducer (inputs, command line, expected vs. actual).
3. Your assessment of impact (crash / memory corruption / DoS / info leak).
4. Whether you believe a CVE should be requested.

## Response timeline

- **Acknowledgment**: within 72 hours.
- **Initial triage**: within 7 days (severity, affected versions, fix path).
- **Fix or mitigation**: aim for 30 days for High/Critical, 90 days for
  Medium/Low. Longer timelines are possible for complex issues — we'll keep
  you informed.
- **Public disclosure**: coordinated with the reporter, typically after a fix
  ships in a tagged release. Credit is given in the release notes unless you
  prefer to remain anonymous.

## Supply-chain guarantees

Every tagged release ships with:

- **SBOMs** (SPDX + CycloneDX) for both the native release artifacts and the
  `vmaf-mcp` wheel/sdist — attached via `supply-chain.yml`.
- **Sigstore keyless signatures** for native artifacts, Python distributions,
  and the SBOMs — verify with
  `cosign verify-blob --bundle <asset>.bundle <asset>`.
- **Distinct SLSA L3 provenance** for the native and Python subjects — generated
  by `slsa-github-generator`; verify with `slsa-verifier`.
- **PyPI PEP 740 attestations** for `vmaf-mcp`, published through the exact
  `VMAFx/vmafx` Trusted Publisher with no long-lived repository token.

These are the acceptance criteria for the D12 "signed releases" gate; a
release without all three is a blocker per `/prep-release`.

## Known non-vulnerabilities

- VMAF is a **quality metric**, not an authentication / crypto / sandbox
  system. Inputs are untrusted video files. Parsing bugs in third-party
  codec libraries (libavcodec, etc.) are routed to those projects; we
  consume them via pkg-config and do not fork their parsers.
- Numerical drift between CPU and GPU backends of up to 2 ULP is a design
  accommodation (see `docs/principles.md` §3 and decision D10), not a
  security issue.
