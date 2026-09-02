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

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **v1.0.0 on a fresh number line** | Honest: it *is* the first release. No collision with Netflix's tag namespace. Clear separation of fork identity from upstream baseline. Ordinary SemVer from a real 1.0.0. | `libvmaf.pc` appears to go 3.2.1 -> 1.0.0 to anyone reading master, and the product number no longer echoes the SONAME. | **Chosen.** The apparent regression is in a version no release ever advertised, and the SONAME/product split is now documented and commented at the source. |
| Continue at v3.2.1 (ADR-1127 as written) | No apparent version regression; matches the current in-tree markers. | Mints the fork's first tag one patch above a Netflix tag the fork never released and does not contain; permanently entangles the fork's number line with upstream's; every future release must dodge upstream's tags. | Rests on a false premise — there is no v3.2.0 fork release to patch. |
| Start at v4.0.0 | Monotone above every inherited tag, so no ordering surprise for anyone who fetched upstream tags into the same namespace. | Claims three major versions of fork release history that never happened; still shares Netflix's namespace and will collide again when upstream reaches 4.x. | Buys only cosmetic monotonicity and keeps the collision problem. |
| Keep `vX.Y.Z-lusoris.N` | Encodes the upstream baseline in the tag. | Already rejected by ADR-1127 for non-standard ordering and coupling to upstream. | Out of scope; ADR-1127's reasoning still holds. |

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
  red process gate now blocks merge where it previously did not.
- **Neutral / follow-ups**: three items remain **repo-admin actions outside any
  code PR** and each is a hard blocker for an actual release —
  (1) create a release-bot identity so release PRs receive check runs at all
  (release-please currently authenticates with `GITHUB_TOKEN`, and GitHub
  suppresses follow-on workflow events from that token, so the sole required
  context can never report and no release PR is mergeable);
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
- [Research-1151](../research/1151-release-please-audit-2026-09-02.md)
- [ADR-1127](1127-single-semver-release-stream.md) — superseded by this ADR
- [ADR-1128](1128-fragment-owned-release-cuts.md) — fragment-owned CHANGELOG
- [ADR-0313](0313-ci-required-checks-aggregator.md) — absent-means-pass semantics
- Source: `req` — paraphrased: the fork's first release is 1.0.0 on a fresh VMAFx number line; every existing tag belongs to Netflix upstream history and the fork has never released.
- Source: `req` — paraphrased: cut the release only after the fork's trained models are retrained for the Netflix VMAF v1.0.16 default; the pipeline must be correct and idle until then.
