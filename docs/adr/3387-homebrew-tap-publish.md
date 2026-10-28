<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3387: The release job publishes the Homebrew formula as a pull request on Vidiko/homebrew-tap

- **Status**: Accepted
- **Date**: 2026-10-29
- **Deciders**: Lusoris (ledger vmafx-2319-package-channels, issue #2319)
- **Tags**: release, packaging, supply-chain, ci

## Context

[ADR-3317](3317-distribution-manifest.md) generates a Homebrew formula from
`packaging/manifest.toml`, but nothing delivers it to users. Homebrew installs
third-party formulae from a tap, a repository named `homebrew-<name>`. The
maintainer decided (ledger `vmafx-2319-package-channels`, paraphrased) that
the tap lives at `Vidiko/homebrew-tap`, that the release job publishes the
formula after a final tag, and that apt and rpm repositories move to
milestone 0.2.0 with their own hosting and signing-key decision. The tap is a
REUSE repository whose CI requires a `Signed-off-by` trailer on every commit.

## Decision

- `distribution.py homebrew` writes `Formula/vmafx.rb`: the generated formula
  below the tap's licence header. `distribution.py source-url` prints the
  source archive the formula names, so the job downloads and hashes exactly
  that file.
- The `homebrew-tap` job in `.github/workflows/supply-chain.yml` runs after
  `attach-to-release`, for final versions only, in the `release-publish`
  environment. It mints a token of the existing `vmafx-release-bot` app scoped
  to `Vidiko/homebrew-tap` with contents and pull-request write.
- `scripts/release/publish_homebrew_tap.py` opens a pull request (branch
  `vmafx-<version>`, commit through the contents API with the bot's sign-off)
  and never pushes to the tap's default branch; the tap's required checks
  decide the merge. A tap that already holds the same formula is left alone;
  a rerun of the same version updates its own branch and pull request.

## Alternatives considered

| Option | Why not chosen |
| --- | --- |
| Push the formula straight to the tap's default branch | Skips the tap's own checks (REUSE, sign-off, `brew style`); a bad formula reaches users before any check runs. |
| `brew bump-formula-pr` in a macOS job | Needs a macOS runner and a formula already in the tap; the first release has none, and the generator already writes the whole file. |
| A new app or a personal access token for the tap | A second credential to rotate; the release bot already signs release commits and only needs an installation on the Vidiko organisation. |
| Publish release candidates too | A candidate formula would become what `brew install` gets; candidates stay on the GitHub release. |

## Consequences

- `brew install Vidiko/tap/vmafx` works once the first formula pull request
  merges; the channel's `state` in the manifest changes to `published` then.
- Until the release bot is installed on the Vidiko organisation, the token
  step fails by name and the rest of the release is already attached.
- The job needs network access to the forge archive; a download failure fails
  the job, never a formula with a wrong digest.

## Supply-chain impact

- **New dependencies**: none. The job uses actions already pinned in the
  workflow (`checkout`, `setup-python`, `create-github-app-token`).
- **Credentials**: no new secret; the existing release-bot app gains one
  installation, scoped to one repository.

## SBOM delta

None: no shipped component changes.

## References

- Issue #2319; [ADR-3317](3317-distribution-manifest.md).
- Ledger `vmafx-2319-package-channels` (maintainer answer, 2026-10-28, paraphrased above).
