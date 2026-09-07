<!-- markdownlint-disable MD013 MD060 -->

# ADR-1201: Cut release candidates before the final 1.0.0

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: release, ci, supply-chain

## Context

The fork has never released. [ADR-1151](1151-vmafx-first-release-1-0-0.md) set the
pipeline up to cut a single first release straight onto `1.0.0`, and
[ADR-1127](1127-single-semver-release-stream.md) fixed the version scheme as ordinary
`vMAJOR.MINOR.PATCH`, independent of upstream Netflix.

Going from "never released" to "1.0.0 final" in one step is a poor bet for a fork that
ships four GPU backends, an FFmpeg patch stack, an MCP server and a tiny-AI surface. The
maintainer's direction is to cut **4–8 release candidates first**.

The pipeline could not do that. Six separate places refused or mishandled a prerelease, and
none of them was a policy statement — each was a guard written when a prerelease could only
be a mistake:

1. `scripts/release/verify-release-version.sh` accepted only `^v<major>.<minor>.<patch>$`.
2. The same script's release-marker extractor matched `[0-9]+\.[0-9]+\.[0-9]+`, so it read
   `1.0.0-rc.1` as `1.0.0` and then reported the marker as disagreeing with the tag it
   actually matched.
3. `supply-chain.yml` exited 1 on `prerelease == true`.
4. …and separately required the GitHub release to have `prerelease == false`.
5. `docker-publish-production.yml` had its own copy of the same rejection.
6. `docker-publish-operator-node.yml` had a third copy.

There was also a latent trap. The ADR-1151 contract gate allows the one-shot `release-as`
and `bootstrap-sha` fields only "until the manifest reaches 1.0.0", and decides that with
`sort -V`. But `printf '1.0.0\n1.0.0-rc.1\n' | sort -V | head -1` returns `1.0.0` — so the
gate would have concluded the RC line had already reached 1.0.0 and failed **every RC
build** while `release-as` was legitimately still present.

## Decision

We will cut `v1.0.0-rc.N` release candidates before `v1.0.0`.

**The accepted prerelease shape is deliberately narrow**: `rc` only, a dotted integer only,
no leading zero. `v1.0.0-rc.1` and `v1.0.0-rc.12` pass; `v1.0.0-beta`, `v1.0.0-rc`,
`v1.0.0-rc.01`, `v1.0.0-rc.1.2` and `v1.0.0-RC.1` are refused. SemVer permits all of those;
the fork ships exactly one prerelease channel, and every extra accepted shape is another way
to mis-tag a release.

**Prereleases are published, not rejected — but the tag and the release flag must agree.**
Each publishing workflow now checks that an `-rc.N` tag is marked `prerelease` and a final
tag is not. The blanket rejection is gone; the consistency check replaces it, because the
genuinely dangerous states are the mismatched ones: an RC published as stable becomes
`latest` for every consumer, and a final published as a prerelease silently never does.

**A release candidate can never take the `latest` moving tag.** GitHub's `/releases/latest`
already excludes prereleases, so the existing resolution could not have picked an RC — but
the failure is severe and silent for anyone pulling `latest`, so `docker-publish-production`
refuses the suffix explicitly rather than relying on one endpoint's semantics staying that
way.

**The contract gate treats a prerelease manifest as pre-1.0.0.** A manifest carrying any
`-` suffix keeps the one-shot fields allowed, checked before the `sort -V` comparison that
would otherwise misfire. Enforcement begins at the final `1.0.0`.

`release-please-config.json` moves to `prerelease: true`, `prerelease-type: rc`, and
`release-as: 1.0.0-rc.1`. Cutting the final release is then two edits: `prerelease: false`
and `release-as: 1.0.0`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `-rc.N` prereleases through the existing pipeline (**chosen**) | One number line, one channel; RCs are real published artifacts exercising the whole supply chain — signing, SBOM, images — before the final cut | Six guards to relax, each needing its replacement check | — |
| Go straight to `1.0.0` | No work | Ships a first release of a four-GPU-backend fork with no rehearsal of the release path itself. The pipeline has never cut anything; the first cut *is* the risky one | The rehearsal is the point |
| `0.9.x` betas, then `1.0.0` | Uses the ordinary triple, so no guard changes at all | `0.9.x` reads as a real version and lands on the stable channel — it would take `latest`. And ADR-1127 already spent the pre-1.0 number space | Semantically wrong, and worse than the guard work it avoids |
| Draft (unpublished) releases as RCs | Nothing reaches consumers | A draft never triggers `release: published`, so none of the publishing pipeline runs — the exact thing an RC exists to exercise | Rehearses nothing |
| Loosen the tag regex to full SemVer | Standards-compliant | Accepts `-beta`, `-alpha.1+build`, `-rc` bare and every other shape, none of which this project ships. More ways to mis-tag, no benefit | Narrow beats permissive for a single-channel project |

## Consequences

- **Positive**: the release path can be rehearsed 4–8 times against real published artifacts
  before the final cut. Every RC exercises signing, SBOM, SLSA provenance and image
  publication exactly as the final release will.
- **Negative**: the final cut now needs two deliberate config edits (`prerelease: false`,
  `release-as: 1.0.0`). That is a step someone can forget; the contract gate catches the
  second half by starting to enforce one-shot-field removal the moment the manifest loses
  its suffix.
- **Neutral / follow-ups**: five new cases in
  `scripts/release/tests/test-verify-release-version.sh` pin the accepted and rejected tag
  shapes (25 passing, was 18). PyPI accepts prereleases natively and pip will not install
  them without `--pre`, so no separate guard is needed there.

## References

- req: maintainer direction, paraphrased — cut at least four to eight release candidates
  before the final 1.0.0.
- [ADR-1151](1151-vmafx-first-release-1-0-0.md) — the one-shot cutover fields and the
  contract gate this retargets.
- [ADR-1127](1127-single-semver-release-stream.md) — the ordinary-SemVer version scheme this extends
  with exactly one prerelease channel.
