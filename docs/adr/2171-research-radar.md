<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2171: A weekly research radar over public video-quality sources

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: research, ci, governance, licence

## Context

New quality metrics, reference implementations, datasets and standards appear
continuously, and several of them bear on open work: reference conformance (RC7,
issue 2286), new metrics (milestone 1.3), A/B and training data (1.4) and the next model
generation (1.5). Until now they were found by chance. Their licences vary from
permissive to none, and an unlicensed repository cannot be used as more than a local
comparison; a patent can sit under a published method whatever the code licence says.
The maintainer decided on a weekly cadence, a public source registry, triage into
backlog issues per milestone, a recurring epic, and a licence and patent gate. Code
without a licence is used only as an oracle run locally and as the reference for a
clean-room re-implementation written from the paper, and for the most valuable such
repositories the project asks the authors, once and politely, to add a licence.

## Decision

We will keep a public registry of sources (`docs/research/radar/sources.yaml`) that
names organisations, repositories, queries, venues, standards documents and datasets
and never people. A scheduled workflow (`research-radar.yml`, Mondays 06:00 UTC, hosted
runner) runs `scripts/research/radar_collect.py`, which uses no model and keeps no
state, and files one digest issue per week labelled `research-radar`; an unchanged feed
files nothing and an unreadable source is named in the digest. Triage is a documented
agent-lane procedure (`docs/research/radar/triage.md`), not a CI job: each item gets a
class, a licence class and a patent note, and becomes a comment or an issue in the
right milestone. An adopted metric follows one shape: a CPU reference written from the
paper, GPU twins and SIMD under the exact-twin contract, conformance against the
original run locally as an oracle, a patent check before work starts, and a speed
comparison recorded as a benchmark row, never claimed before it is measured. A recurring
epic links the registry, the digests, the procedure and each sweep.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Monthly sweep | Less noise | A relevant release waits up to a month | The maintainer chose weekly (Q-072) |
| Model-written digest in CI | Summaries, early classification | A secret and a paid or rate-limited dependency in CI; non-deterministic output | The collector stays deterministic; judgement happens in the triage lane |
| Stateful "seen" store | Exact de-duplication | A store to host, back up and repair | Exact weekly windows partition time without state |
| Watch people's profiles | Early signal | Profiles individuals in a public tracker | Sources only; a private contact list stays outside the repository |
| One-off sweep, no workflow | No maintenance | Drifts out of date, as the first answer did | Only a recurring job keeps the registry current |
| Vendor unlicensed code | Fastest | No right to copy or ship it | Oracle only, clean-room re-implementation, ask for a licence |

## Consequences

- **Positive**: new research reaches the backlog within a week with a licence class and
  a patent note; the adopted-metric shape is stated once and repeated in every issue.
- **Negative**: one more weekly job and a registry to keep current; a digest is only as
  good as the queries (OpenAlex matches titles and abstracts, arXiv matches abstracts).
- **Neutral / follow-ups**: the first sweep and the licence requests are recorded in
  the epic. The collector's `github_owner` listing reads the 100 most recently pushed
  repositories of an owner and checks releases of at most ten active repositories per
  owner per run. A generic version belongs in the standards engine
  (a research and upstream radar module); it is proposed there, not built here.

## References

- Decision Q-072 (weekly sweep) and Q-073 (oracle only, clean-room, ask authors for a
  licence), maintainer, 2026-10-07.
- [ADR-0448](0448-active-upstream-monitoring-discipline.md): watchers for upstream
  artefacts that block a row; this radar watches research, not blockers.
- [ADR-1227](1227-short-workflow-display-names.md): workflow display names.
