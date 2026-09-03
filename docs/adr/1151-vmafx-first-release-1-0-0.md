<!-- markdownlint-disable MD013 MD060 -->
# ADR-1151: Cut the fork's first release as v1.0.0 on a fresh number line

- **Status**: Accepted
- **Date**: 2026-09-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: release, semver, automation, ci, docs

## Context

[ADR-1127](1127-single-semver-release-stream.md) put VMAFx on one ordinary SemVer
stream "starting with `v3.2.1`". That starting point rested on a premise that does
not hold: it treated the repository's 3.2.x source baseline as if the fork had
already released 3.2.0.

The fork has never released. `gh api repos/VMAFx/vmafx/releases` returns zero
releases, and there are no fork-made tags: all 27 tags on `origin` are inherited
Netflix ones, topping out at `v3.0.0`. The locally visible `v3.1.0` / `v3.2.0`
came from the `upstream` remote — `v3.2.0` is Netflix's
`libvmaf/meson: bump SONAME to 3.2.0` — and `git merge-base --is-ancestor v3.2.0
origin/master` is false, so none of them is an ancestor of `master`. The 3.2.x
manifest baseline was a source-version alignment with Netflix's SONAME
(`docs/state.md`: "Source-only; no release performed"), not a release. Releasing
`v3.2.1` would therefore mint the fork's first-ever tag inside Netflix's tag
namespace, one patch above a tag the fork never published and does not contain.

An audit of the pipeline that would have produced that tag also found it unable
to release correctly at all. Release PRs receive zero check runs, so the sole
required context can never report on them; nothing proved the hand-run changelog
cut had happened before a tag was published; the `release-publish` deployment
environment referenced by twelve write-bearing jobs did not exist and was being
auto-created with no protection rules; and the persistent `release-as` override
would have pinned every future release to the same version. Those repairs and
this number-line decision are one change, because the version the pipeline emits
is the thing being repaired.

## Decision

The fork's **first release is `v1.0.0`**, on a number line that starts at the
fork and is unrelated to Netflix's. `release-please-config.json` carries a
one-shot `release-as: "1.0.0"` and `.release-please-manifest.json` is reset to
`0.0.0`, so `0.0.0 -> 1.0.0` is a monotone forward bump that `release-as` merely
confirms rather than a forced downgrade from 3.2.0. Both `release-as` and
`bootstrap-sha` are one-shot: `scripts/release/rollover-changelog-fragments.sh`
deletes them in the release-merge PR, and the `Release Script Contract
(ADR-1128)` gate refuses to let either return once the manifest reaches 1.0.0.
Any later forced version uses a `Release-As: X.Y.Z` commit footer, which is
inherently one-shot and cannot rot. This supersedes ADR-1127's choice of `v3.2.1`
as the first release; ADR-1127's substance — one independent ordinary-SemVer
stream, one release PR, publication gated on an authenticated operator — stands
unchanged.

**The product version and the libvmaf ABI SONAME are independent, and the 1.0.0
cut does not touch the SONAME.** release-please owns the *product* version: the
`vX.Y.Z` tag, `core/meson.build`'s `project(version:)` (which becomes
`libvmaf.pc`'s advertised version), the three fork-local Python distributions
(`ai/`, `dev-llm/`, `mcp-server/vmaf-mcp/`), the compatibility `vmaf` Python
package, and the Helm chart's `appVersion`. The ABI SONAME is the separate
hardcoded `vmaf_soname_version = '3.0.0'` at `core/meson.build:19`, consumed as
Meson's `version:` / `soversion:` and producing `libvmaf.so.3`. It is hand-bumped
only on an ABI break and is **not** reset by this decision — `libvmaf.so` keeps
its 3.x line while the product goes to 1.0.0. The one visible consequence is that
`libvmaf.pc` moves from an advertised 3.2.1 to 1.0.0; since no release ever
shipped that 3.2.1, no consumer can be pinned to it.

**release-please authenticates as a GitHub App, and the six process gates
become required with a machine-generated-PR exemption.** `GITHUB_TOKEN` cannot
be the release identity: GitHub suppresses follow-on workflow events for
anything that token creates, so release PRs received zero check runs and the
sole required context could never report. `release-please.yml` now mints an
installation token with `actions/create-github-app-token` and routes both
release-please invocations and both read-only `gh api` probes through it; the
job's own `GITHUB_TOKEN` drops to `contents: read`. The App and its two secrets
(`RELEASE_BOT_APP_ID`, `RELEASE_BOT_PRIVATE_KEY`) are a repo-admin action, and
until they exist the workflow fails on its first step rather than falling back
to `GITHUB_TOKEN` and quietly recreating an unmergeable release PR.

Promoting the six `rule-enforcement.yml` gates into the aggregator's `required`
array would, on its own, make every release PR permanently red: four of them
grade *authoring discipline* that a generated PR structurally cannot supply —
no ADR-0108 checklist in a rendered-changelog body, and a version-marker diff
that path-maps `mcp-server/vmaf-mcp/pyproject.toml` to a mandatory `docs/mcp/`
edit. Those four therefore consult `scripts/ci/release-pr-exempt.sh` and skip
their work step on a machine-generated release PR, reporting green rather than
absent. The predicate requires a bot author *and* a `release-please--` head ref,
so branch naming alone cannot disarm a required gate. `Release Script Contract`
and `ADR Number Collision Guard` stay armed on release PRs, and the former also
runs the predicate's own test suite, so the exemption cannot rot unnoticed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **v1.0.0 on a fresh number line** | Honest: it *is* the first release. No collision with Netflix's tag namespace. Clear separation of fork identity from upstream baseline. Ordinary SemVer from a real 1.0.0. | `libvmaf.pc` appears to go 3.2.1 -> 1.0.0 to anyone reading master, and the product number no longer echoes the SONAME. | **Chosen.** The apparent regression is in a version no release ever advertised, and the SONAME/product split is now documented and commented at the source. |
| Continue at v3.2.1 (ADR-1127 as written) | No apparent version regression; matches the current in-tree markers. | Mints the fork's first tag one patch above a Netflix tag the fork never released and does not contain; permanently entangles the fork's number line with upstream's; every future release must dodge upstream's tags. | Rests on a false premise — there is no v3.2.0 fork release to patch. |
| Start at v4.0.0 | Monotone above every inherited tag, so no ordering surprise for anyone who fetched upstream tags into the same namespace. | Claims three major versions of fork release history that never happened; still shares Netflix's namespace and will collide again when upstream reaches 4.x. | Buys only cosmetic monotonicity and keeps the collision problem. |
| Keep `vX.Y.Z-lusoris.N` | Encodes the upstream baseline in the tag. | Already rejected by ADR-1127 for non-standard ordering and coupling to upstream. | Out of scope; ADR-1127's reasoning still holds. |
| **Release identity: GitHub App installation token** | Non-expiring, scoped to two permissions on one repo, minted and revoked per run, not tied to a person. PRs it opens trigger CI. | Requires a one-time App creation the workflow cannot perform itself. | **Chosen.** The one-time setup is bounded; the alternatives are either unusable or permanent debt. |
| Release identity: personal access token | No App to create. | Ties the release stream to one human's account, expires and needs rotation, and carries that human's full scope. | Rejected — a release pipeline that dies when one person's token lapses is not a pipeline. |
| Release identity: keep `GITHUB_TOKEN`, admin-merge every release PR | Zero setup. | Contradicts the no-`--admin`-by-default rule and defeats the whole point of making the release-critical checks required. | Rejected — it is the failure this ADR exists to remove. |
| **Release-PR gate exemption: shared bot+head-ref predicate** | One script, one behaviour, testable and locally runnable; jobs report green rather than absent, so a genuine path-filter skip stays distinguishable. | A fifth gate added later must remember to consult it. | **Chosen.** The predicate ships with a test the always-armed Release Script Contract job runs. |
| Head-ref-only exemption | Simpler — no author plumbing. | Any contributor could name a branch `release-please--x` and skip four required gates. | Rejected — an exemption a stranger can claim is not a gate. |
| Skip the whole job on a release PR (`if:` at job level) | Fewer moving parts. | The check reports *absent*, which the aggregator's absent-means-pass rule then cannot distinguish from a path-filter skip. | Rejected — it re-introduces the ambiguity `mustReport` exists to close. |
| Teach release-please to emit a compliant PR body | No exemption at all. | The body is a release-please template; the doc-substance failure is diff-driven and no body text fixes it. | Rejected — solves at most half the problem. |

## Consequences

- **Positive**: the first release is a real 1.0.0 and cannot be mistaken for a
  patch on Netflix's line. The one-shot cutover fields are now gated rather than
  merely documented, so `release-as` cannot silently pin later releases. The
  changelog cut, the publication environments, and the release PR's own CI are
  all fail-closed preconditions of a tag instead of operator memory.
- **Negative**: `libvmaf.pc` advertises 1.0.0 while `libvmaf.so.3` keeps a 3.x
  SONAME, which reads as odd until the split is explained; the explanation now
  lives here, in `docs/development/release.md`, and in a comment at
  `core/meson.build:19`. Making the six rule-enforcement gates required means a
  red process gate now blocks merge where it previously did not, and four of
  them carry a machine-generated-PR exemption that a future gate author has
  to remember to wire up. `release-please.yml` is hard-down until the
  release-bot App exists — intentional, but it does mean the release PR
  stops being refreshed in the interim.
- **Neutral / follow-ups**: three items remain **repo-admin actions outside any
  code PR** and each is a hard blocker for an actual release —
  (1) create the release-bot GitHub App and its `RELEASE_BOT_APP_ID` /
  `RELEASE_BOT_PRIVATE_KEY` secrets — the workflow half is done here and
  `release-please.yml` fails loudly on its first step until they exist;
  (2) create the `release-publish` and `pypi-publish` environments with a
  required reviewer and a `v*` tag deployment policy — `supply-chain.yml` now
  fails closed until they exist;
  (3) decide whether master's coordinated version markers are reset from 3.2.1
  ahead of the cut or left for the release PR to rewrite.
  The release itself is deliberately deferred until the fork's trained models
  are retrained against the Netflix VMAF v1.0.16 default; nothing in this ADR
  triggers one.

## References

- [release-please manifest-releaser docs — `release-as` is persistent and must be removed after the release PR merges](https://github.com/googleapis/release-please/blob/main/docs/manifest-releaser.md)
- [release-please config schema — `release-as` is marked DEPRECATED in favour of a `Release-As` commit footer](https://github.com/googleapis/release-please/blob/main/schemas/config.json)
- [GitHub Actions — events from `GITHUB_TOKEN` do not trigger further workflow runs](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow)
- [`actions/create-github-app-token` — mint a scoped installation token per run](https://github.com/actions/create-github-app-token)
- [Research-1151](../research/1151-release-please-audit-2026-09-02.md)
- [ADR-1127](1127-single-semver-release-stream.md) — superseded by this ADR
- [ADR-1128](1128-fragment-owned-release-cuts.md) — fragment-owned CHANGELOG
- [ADR-0313](0313-ci-required-checks-aggregator.md) — absent-means-pass semantics
- Source: `req` — paraphrased: the fork's first release is 1.0.0 on a fresh VMAFx number line; every existing tag belongs to Netflix upstream history and the fork has never released.
- Source: `req` — paraphrased: cut the release only after the fork's trained models are retrained for the Netflix VMAF v1.0.16 default; the pipeline must be correct and idle until then.
