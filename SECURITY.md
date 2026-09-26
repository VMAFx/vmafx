# Security Policy

## Supported versions

VMAFx is preparing its first `1.0` release candidate. As of 2026-09-08,
[the active repository has no published releases](https://github.com/VMAFx/vmafx/releases).
There are no supported stable VMAFx release lines yet. Version strings inherited
from libvmaf or Netflix tags do not identify a released VMAFx support line.

Report problems against the current `master` commit and include its full SHA.
Development fixes are made on the current code; this is not a promise of
backports to unannounced releases. The first release's notes will identify its
support policy. Reports about Netflix's releases belong to
[Netflix/vmaf's security process](https://github.com/Netflix/vmaf/security);
we coordinate when a problem also affects this fork.

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
- **Fix or mitigation**: address Critical issues urgently and aim for no more
  than 30 days for High/Critical reports, 60 days for Medium and 90 days for Low.
  Publicly known Medium-or-higher issues must be patched and the fix released
  within 60 days of becoming public. Interim mitigations do not replace that
  release obligation. Keep the reporter informed of the plan and any missed target.
- **Public disclosure**: coordinated with the reporter, typically after a fix
  ships in a tagged release. Credit is given in the release notes unless you
  prefer to remain anonymous.

These are the current response policy and targets, not a statement that past
reports met them. Badge assessments need actual response and remediation records.

## Release verification

The [supply-chain workflow](.github/workflows/supply-chain.yml) defines intended
SBOM, signature, provenance and package-attestation steps. Configuration alone
is not proof that these outputs were produced or verified for a release.
Because no VMAFx release has been published at this assessment date, this policy
does not claim an achieved SLSA level, published attestations or universal
signed-artifact coverage.

For a future release, use its actual asset list and verification receipts with
the [release verification guide](docs/development/release.md). Missing expected
outputs block release acceptance; do not infer their existence from this policy
or from the presence of a workflow file.

## Report scope

- The core quality metric is not a sandbox boundary. Treat video inputs as
  untrusted; optional network services and model verification have their own
  security-relevant behavior. Reports involving external codec libraries are
  coordinated with their projects when appropriate.
- Backend numerical tolerances vary by feature; the
  [cross-backend gate](docs/development/cross-backend-gate.md) is the reference.
  A score difference alone does not establish a security defect. Describe the
  affected input, behavior and impact so it can be routed appropriately.

VMAFx holds the [OpenSSF Best Practices passing badge](https://www.bestpractices.dev/projects/14549).
The [badge record](docs/development/best-practices-assessment.md) lists the
submitted answer and its evidence for each criterion.
