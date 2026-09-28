<!-- markdownlint-disable MD013 MD060 -->
# ADR-1352: Shift the first-release candidate mapping by one

- **Status**: Accepted
- **Date**: 2026-09-28
- **Deciders**: lusoris
- **Tags**: release, rc

## Context

[ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) gave each
first-release candidate one responsibility: `v1.0.0-rc.1` (RC1) closes
release-blocking correctness and proves the outside-tester report path,
`v1.0.0-rc.2` (RC2) owns benchmarks, profiling and tuning, and `v1.0.0-rc.3`
(RC3) owns the one-shot real retrain.

`v1.0.0-rc.1` was published on 2026-09-27 (`CHANGELOG.md`). Since then master
has taken Renovate and other dependency updates plus correctness fixes, and the
rc.1 release verification found new defects, such as the build-tree RUNPATH on
the released `vmaf` binary (`T-RELEASE-NATIVE-RUNPATH-BUILD-TREE-2026-09-27` in
[`docs/state.md`](../state.md)). Testers should get that train before any
benchmark work starts, so the maintainer is cutting `v1.0.0-rc.2` as a second
correctness and stabilisation candidate with no benchmark evidence.

ADR-1341 already allows a repair candidate, but its phase names would then stop
matching the tags. rc.2 would be an "RC1 repair", benchmarks would run on rc.3
under the name RC2, and the retrain would run on rc.4 under the name RC3. The
maintainer asked for every milestone and epic to move by one instead.

## Decision

We will shift ADR-1341's candidate-to-tag mapping by one candidate, so that each
phase number matches its tag:

| Candidate | Responsibility | Exit evidence |
| --- | --- | --- |
| `v1.0.0-rc.1` (RC1) | Correctness and the outside-tester report path (published, unchanged). | As in ADR-1341. |
| `v1.0.0-rc.2` (RC2) | Stabilisation and repair: the dependency and fix train since rc.1. It makes no benchmark, profiling, tuning or training claim. | The RC1 bar on the rc.2 head: no confirmed release blocker, no untriaged `docs/state.md` row, required checks green on the exact head, and the tester report path still producing reproducible reports. |
| `v1.0.0-rc.3` (RC3) | Benchmarks, profiling and tuning (ADR-1341's RC2). | ADR-1341's benchmark evidence, pinned to the rc.3 artifact. |
| `v1.0.0-rc.4` (RC4) | The one-shot real retrain and the remaining tiny-AI training (ADR-1341's RC3). | ADR-1341's retrain evidence, pinned to the rc.4 artifact. |

Final `v1.0.0` follows accepted RC4 evidence and any required repair candidate.

This amends only the tag mapping. Everything else in ADR-1341 stays: the phase
order, exact-head evidence and revalidation after later merges, immutable tags,
the rule that benchmarking never moves into a correctness candidate and that
retraining never starts before benchmark evidence is accepted, and the policy
that Renovate and version updates are not frozen.

Ledger rows in [`docs/state.md`](../state.md) keep their kind of work and change
their label. Performance and backend-acceleration rows move from RC2 to RC3.
Training and model-validation rows move from RC3 to RC4. Correctness fixes aimed
at the next candidate stay RC2.

Two identifiers keep their old names. `tools/rc1-tester/` keeps its name because
it serves both RC1 and RC2. The backlog IDs `T-RC2-BENCH-TUNE` and
`T-RC3-MODEL-RETRAIN` are stable identities under
[ADR-1303](1303-backlog-checklist-tracker-schema.md), and renaming them would
break lookups.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep ADR-1341's numbering and call rc.2 an "RC1 repair candidate" | No document or tracker churn. ADR-1341 already permits repair candidates. | Phase names stop matching tags: benchmarks ship on rc.3 as "RC2" and the retrain on rc.4 as "RC3". | The maintainer wants milestone and epic numbers to follow the tags. |
| Skip rc.2 and start benchmarking on the next candidate | One fewer candidate before the final release. | The dependency and fix train reaches testers only inside a benchmark candidate, and benchmark results mix with fixes no tester has checked. | The dependency and fix train must reach testers as its own candidate. |
| Shift the mapping by one so that each phase number matches its tag (**chosen**) | Tag, milestone, epic and phase numbers agree. rc.2 gets a checkable exit bar. | One more candidate before the final release, and one pass over the forward-looking documents, the tester tool inventory and the ledger. | The cost is a single mechanical pass, and the names no longer need translating. |

## Consequences

- **Positive**: Anyone can read the phase from the tag. rc.2 gives testers the
  dependency and fix train with the same report path and exit bar as rc.1.
- **Negative**: The final release is one candidate further away. Historical
  records keep the old names and must be read with this mapping. These include
  ADR-1341's body, ADR-1342, ADR-1346 and ADR-1348, earlier
  `docs/rebase-notes.md` entries, and `docs/state.md` update notes dated before
  2026-09-28. If a later stage needs its own repair candidate, the names and
  tags drift apart again unless a further amendment shifts them.
- **Neutral / follow-ups**: The maintainer renames the GitHub milestones and
  moves epics [#1245](https://github.com/VMAFx/vmafx/issues/1245) (benchmarks)
  to RC3 and [#1246](https://github.com/VMAFx/vmafx/issues/1246) (retrain) to
  RC4. GitHub state is outside this change. The same change updates the
  release guide, roadmap, retrain runbook, tester guide, affected model card,
  dependency-bot policy, ledger classification, `AGENTS.md` §11 with its
  compiled projections, and the `tools/rc1-tester` inventory. This decision adds
  no dependency, build-time fetch, runtime surface or SBOM component.

## References

- `req` (verbatim): "move all the milestones and epics by +1"
- `req` (verbatim): "cut and release rc 2"
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) — the
  sequence this ADR amends.
- [ADR-1348](1348-release-candidate-prerelease-versioning.md) — release-please
  numbers `1.0.0-rc.2` to `1.0.0-rc.4` without a manual footer.
- [ADR-1201](1201-release-candidates-before-1-0-0.md) — candidate tag and
  publication mechanics.
- [ADR-1303](1303-backlog-checklist-tracker-schema.md) — stable backlog IDs.
