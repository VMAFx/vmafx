<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3292: Restart the release line at 0.1.0; 1.0.0 becomes the first stable release

- **Status**: Accepted
- **Date**: 2026-10-12
- **Deciders**: lusoris
- **Tags**: release, governance, roadmap, go, supply-chain

## Context

VMAFx published three candidates of 1.0.0 (`v1.0.0-rc.1` on 2026-09-27, `rc.2`
on 2026-09-28, `rc.3` on 2026-10-07) under the candidate maps of
[ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md),
[ADR-1421](1421-rc3-rc8-candidate-map.md),
[ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md),
[ADR-1868](1868-candidate-map-2026-10-05.md),
[ADR-1880](1880-format-envelope-device-targets.md),
[ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md) and
[ADR-2342](2342-rc-map-amendment-2026-10.md). By then 1.0.0 carried RC4 to RC9
(new API, device-frame import, deduplication, capability tables, benchmarks,
retraining, cloud-native deployment), and the Rust core that is meant to
replace the C and C++ engine was scheduled after it, from 2.1. A stable 1.0.0
would have frozen an interface that the Rust phases still change.

The operator decided to restart the version line below 1.0, to make 1.0.0 the
first stable release, and to put Rust phase P3 and an interface freeze in front
of it (References). Three tools are affected:

- release-please, which owns the root version, the manifest and the tags;
- the Go module `github.com/VMAFx/vmafx`, whose rc tags are already on the
  public module proxy;
- the published PyPI candidates of `vmaf-mcp`.

## Decision

1. **Versions.** The next release is `v0.1.0`. After it, 0.x releases are
   plain minor and patch releases without candidates. Before 1.0.0 a breaking
   change raises the minor (`bump-minor-pre-major: true`), and a `feat` raises
   the minor too (`bump-patch-for-minor-pre-major: false`). Only 1.0.0 runs
   candidates again, starting at `v1.0.0-rc.4`, because rc.1 to rc.3 exist.
   Library SONAMEs keep their own numbers and do not follow the product version.
2. **Cut-over.** `release-please-config.json` switches the root package to
   `versioning: default`, `prerelease: false` and the one-shot
   `release-as: "0.1.0"`. `scripts/release/rollover-changelog-fragments.sh`
   deletes `release-as` in the release-merge pull request, as it already does
   for every one-shot field ([ADR-1151](1151-vmafx-first-release-1-0-0.md)).
   `scripts/release/check-one-shot-fields.sh` (required `Rule Enforcement` job)
   fails when `release-as` or `bootstrap-sha` survives at a final manifest
   version. It also fails when a candidate manifest has neither
   `versioning: prerelease` nor `release-as`.
3. **Withdrawn candidates.** rc.1 to rc.3 stay on the forge with their tags,
   images and assets. Their release notes are marked as withdrawn, and nothing
   is deleted.
4. **Go.** The root `go.mod` of v0.1.0 retracts rc.1 to rc.3 in a block with
   one reason per entry. This ADR claims only the measured scope (next
   section): through the public proxy the retraction has no effect, and the Go
   consumer docs tell users to pin `@v0.1.0`. A new module path is a separate,
   later decision.
5. **PyPI.** `vmaf-mcp` 1.0.0rc1 to rc3 are yanked by the operator after 0.1.0
   is published on PyPI, with a reason that points at 0.1.0.
6. **Map.** The stage content of the earlier maps stays. Their release numbers
   are replaced by the table in [the roadmap](../roadmap.md#releases) and by
   `AGENTS.md` §11:

   | Release | Content |
   | --- | --- |
   | 0.1.0 | RC4 package plus packaging |
   | 0.2.0 | RC5 deduplication and tool consolidation, RC6, RC7, RC8, observability, RC3 carry-over |
   | 0.3.0 | Rust P1a |
   | 0.4.0 | Encoder feedback, embedding, platforms |
   | 0.5.0 | Rust P1b |
   | 0.6.0 | Rust P2 |
   | 0.7.0 | New metrics |
   | 0.8.0 | Metric A/B |
   | 0.9.0 | Next model generation, RC9 retrain |
   | 1.0.0 | Rust P3 and the interface freeze |
   | 1.1 | Integrations, live quality, cloud-native |
   | 1.2 | Rust P4 and P5 |
   | 1.3 | Open subjective studies |
   | 2.0 | Breaking changes, `libvmaf.h` compat removed ([ADR-1852](1852-vmafx-api-redesign.md) D7) |
   | 3.0 | Host C/C++ removed |

This ADR supersedes the candidate maps and release numbering of ADR-1341,
ADR-1421, ADR-1490, ADR-1868, ADR-1880, ADR-2001 and ADR-2342; their stage
content still applies. It amends
[ADR-1127](1127-single-semver-release-stream.md) (one SemVer stream; it now
starts at 0.1.0), ADR-1151 (the first release is 0.1.0, not 1.0.0),
[ADR-1201](1201-release-candidates-before-1-0-0.md) (candidates only for
1.0.0) and [ADR-1348](1348-release-candidate-prerelease-versioning.md)
(prerelease versioning only while a 1.0.0 candidate line is open).

## Measured before the decision

From the scratch proofs of 2026-10-12. Each case ran against fixtures, with no
forge write.

- **release-please 17.6.0** (locked by the action pin `45996ed1f6d0`, v5.0.0)
  was run on its real manifest code with a mock forge:

  | Case | Result |
  | --- | --- |
  | Manifest `1.0.0-rc.3` with the one-shot | 0.1.0 |
  | After the cut, a fix | 0.1.1 |
  | After the cut, a feat | 0.2.0 |
  | After the cut, a breaking feat | 0.2.0 |
  | Without the one-shot | 1.1.0-rc.3 |
  | The configuration before this ADR | 1.0.0-rc.4 |
  | One-shot left in after the cut | 0.1.0 again |

  The last row is why removing the field and gating on it are both mandatory.
- **Go 1.27, scratch `file://` proxy.** With the retraction in v0.1.0,
  `go list -m -versions` shows only v0.1.0, `@latest` resolves to v0.1.0, and
  `go get @v1.0.0-rc.3` prints the per-entry reason. Without it, rc.3 stays
  listed and installs silently.
- **Go, the real proxy.** `proxy.golang.org` still caches Netflix's
  v1.x, v2.x+incompatible and v3.0.0+incompatible under the module path
  (state row `T-INHERITED-NETFLIX-TAGS-GO-LATEST-2026-10-05`,
  [ADR-1805](1805-delete-inherited-netflix-tags.md)). `go list -m @latest`
  resolves to v3.0.0+incompatible. The go command reads retractions from
  that highest version, which has none. The same layout on the scratch proxy
  reproduces this: rc.3 is listed and installs without a warning. The
  retraction therefore applies only in direct mode, or if the proxy ever stops
  serving those versions.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the 1.0.0 line, cut 1.0.0 after RC9 | No renumbering; maps unchanged | 1.0.0 freezes an interface the Rust phases still change; stable would be followed by breaking 2.x soon after | Operator chose the restart (References) |
| Restart at 0.x with candidates for every minor | Same release discipline as before | Candidate cycles for every 0.x minor cost a cycle each with no stability promise to protect | Operator chose plain 0.x minors |
| Delete the rc.1–rc.3 tags and releases | Clean tag list | Deletes published artifacts and images people may have pulled; Go proxy keeps them anyway | Withdrawn notes keep provenance; nothing deleted |
| Set the version by a `Release-As:` commit footer | No configuration change | Footer applies to whichever commit release-please reads; the prerelease versioning strategy would still treat later releases as candidates | The one-shot plus the gate is measured and reversible |
| New Go module path (`/v0` or a new name) | `@latest` would resolve cleanly | Breaks every existing import; nobody has asked for Go consumers yet | Deferred to a later operator question if Go consumers appear |
| 1.0.0 stable at Rust P1a or P2 | Earlier stable | Interface still moves in P3 | Operator chose P3 plus the freeze |

## Consequences

- **Positive**: the version number says what it means: below 1.0, interfaces
  can change. The stable promise comes once the engine underneath it is final.
  The one-shot field cannot silently pin later releases.
- **Negative**: users who installed an rc have to move to 0.1.0, which SemVer
  sorts lower. The Go `@latest` defect stays until the proxy stops serving the
  inherited Netflix versions, or until a new module path is chosen.
- **Follow-ups**: mark the rc.1 to rc.3 release notes as withdrawn. Re-pin
  pelorus once to the new tags. The operator yanks `vmaf-mcp` 1.0.0rc1 to rc3
  after 0.1.0 is on PyPI. Create the freeze-review issue in milestone 1.0.0.
  When the first 1.0.0 candidate is cut, switch the root package back to
  `versioning: prerelease` (ADR-1348).

## References

- Source: operator decision of 2026-10-12, ledger id `vmafx-version-restart-0x`
  (paraphrased): restart below 1.0 with 0.1.0 first, renumber the milestones,
  make 1.0.0 the first stable release once the Rust core has landed and the
  interface is frozen, keep rc.1 to rc.3 as withdrawn, re-pin the storage
  project once.
- Source: ledger id `vmafx-replan-stable-bar` (paraphrased): 1.0.0 needs Rust
  P3 and a reviewed, frozen public interface.
- Source: ledger id `vmafx-replan-release-shape` (paraphrased): 0.1.0 final,
  then plain 0.x minors; candidates only for 1.0.0 from rc.4; prove the
  one-shot on a scratch repository before the configuration change.
- Source: ledger id `vmafx-replan-pypi-rc` (paraphrased): yank the three
  `vmaf-mcp` candidates after 0.1.0 is on PyPI.
- Source: ledger id `vmafx-replan-compat-removal` (paraphrased): the
  compatibility library stays until 2.0.
- Source: ledger ids `vmafx-replan-map-v2` and
  `vmafx-replan-foundation-inversion` (paraphrased): the smaller 0.1.0, the
  0.2.0 foundation release, and every later milestone one minor later.
- Source: ledger id `vmafx-go-latest-0x` (paraphrased): ship the retraction in
  v0.1.0, claim only the measured scope, and tell Go consumers to pin v0.1.0.
- Related: epic #2567 (Rust core), milestone renumbering of 2026-10-12.
