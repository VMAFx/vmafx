<!-- markdownlint-disable MD013 MD060 -->
# ADR-3356: Release workflows authenticate only with the release-bot App token

- **Status**: Accepted
- **Date**: 2026-10-19
- **Deciders**: Lusoris
- **Tags**: ci, release, release-please, security

## Context

[ADR-1151](1151-vmafx-first-release-1-0-0.md) made the release-bot GitHub App
the identity of `release-please.yml`, so the release pull request receives check
runs. [ADR-1171](1171-release-please-credential-gate-warning.md) then let a push
without the App secrets finish green: a `creds` step recorded the missing
secrets, every write step was skipped and a final step reported the idle
pipeline. A later change added a third path: without App secrets the job took a
personal access token from `RELEASE_BOT_TOKEN`, chosen in a "Resolve the
release-bot token" step, and the macOS and Windows tester-bundle workflows
chose between the same two identities.

Praetor's release-token audit requires that every release-please step takes its
token directly from an `actions/create-github-app-token` step of the same job
that has no `if:` and no `continue-on-error:`. The workflow failed the audit,
and the gap was carried as a category C exception
(`.config/lint-exceptions.d/release-token.toml`, expiring 2026-10-29).

## Decision

The App installation token is the only identity of the release workflows.

- `release-please.yml` mints the token in an unconditional step (`id: app-token`)
  and passes `steps.app-token.outputs.token` directly to both release-please
  invocations, the `gh api` probes and the draft-release step. The `creds`
  step, the token-resolving step and the idle-pipeline report are removed.
- `macos-tester-bundle.yml` and `windows-tester-bundle.yml` mint the token
  unconditionally and create the tester prerelease with it; the
  `RELEASE_BOT_TOKEN` fallback is removed.
- Without `RELEASE_BOT_APP_ID` and `RELEASE_BOT_PRIVATE_KEY` every run fails at
  the mint step. This supersedes ADR-1171's green idle path; ADR-1151's rule
  that no release step falls back to `GITHUB_TOKEN` stands.
- The release-token exception is removed in the same change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the personal access token fallback under the exception | Releases still possible when the App key is lost | Fails praetor's audit; the exception expires 2026-10-29; ties the release stream to one person's account | The exception is a deadline, not a state |
| Keep ADR-1171's idle-green path without the token fallback | Master stays green without App secrets | The mint step needs an `if:`, which the audit refuses | Not expressible under the audit |
| App token only, unconditional (chosen) | Passes the audit; one identity; no long-lived credential | A missing or lost App key turns every release run red until the operator restores it | — |

## Consequences

- **Positive**: one identity for every release write; no long-lived credential
  in the release path; praetor's release-token audit passes without an
  exception.
- **Negative**: losing the App private key stops releases and tester bundles
  until the operator generates a new key; the failure is red, not a warning.
- **Neutral / follow-ups**: [ADR-1388](1388-release-pat-mode-gate-exemption.md)'s
  PAT-mode exemption in `scripts/ci/release-pr-exempt.sh` has no producer any
  more; removing it is a separate change. Recovering the App key stays an
  operator act. `docs/development/release.md` and
  `docs/development/tester-image.md` describe the new behaviour.

## Supply-chain impact

- **New dependencies**: none.
- **Removed dependencies**: the `RELEASE_BOT_TOKEN` secret is no longer read.
- **Build-time fetches**: none.
- **Sigstore-signable**: unchanged.
- **CVE surface delta**: narrower; no personal token in the release path.

## SBOM delta

None.

## Carbon / footprint

None measurable.

## References

- ADR-1151, ADR-1171, ADR-1388.
- Controller answer `vmafx-release-token-app-only` (2026-10-19, paraphrased):
  make the App token step unconditional, drop the personal access token
  fallback and the release-token exception, and record the change in an ADR.
- Praetor release-token audit (`praetorctl audit`, check "Release token").
