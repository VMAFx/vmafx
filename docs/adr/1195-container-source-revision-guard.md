<!-- markdownlint-disable MD013 MD060 -->

# ADR-1195: Record and verify which source revision the dev container was built from

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: ci, build, testing, agents

## Context

CLAUDE.md rule 15 makes the `vmaf-dev-mcp` container the default place to do vmaf,
vmaf-tune, AI and MCP work, and tells the operator to "rebuild the container if its image
predates the last `master` sync that touched anything under `core/`, `mcp-server/`, `ai/`,
`tools/vmaf-tune/`, or `dev/`". That instruction is stated in terms of time, and time does
not answer the question it is trying to ask.

On 2026-09-06 the container was rebuilt specifically to pick up the GPU default-model
fixes ([#1307](https://github.com/VMAFx/vmafx/pull/1307),
[#1312](https://github.com/VMAFx/vmafx/pull/1312),
[#1324](https://github.com/VMAFx/vmafx/pull/1324)) so the retrain's GPU smoke could run
against them. The build succeeded. The image was newer than every commit in the
repository, so every timestamp-based reading of rule 15 called it current. It contained
none of the fixes: the build context — the main checkout — was 28 commits behind
`origin/master`, and a build context that is behind produces an image that is
simultaneously the newest thing on disk and missing the work it was built for.

It was caught by accident, because a test file added by one of those PRs was absent.
Nothing else would have caught it. The failure mode had the GPU smoke run instead is the
bad one: a green result, attributed to code that was not in the image, used as evidence
for a retrain gate.

[ADR-1102](1102-phase4b9-container-only-publishing.md) already established the marker that
distinguishes a container build from a host build. It answers "did a container build
this?" It cannot answer "which code was in that container?", and this incident is exactly
the second question.

## Decision

We will make the source revision of the dev container an explicit, checkable fact in two
places.

`dev/Containerfile` records `/etc/vmafx-dev-source` — `source_rev`, `source_ref`,
`source_repo` — written in the **final** stage, from a `VMAFX_SOURCE_REV` build argument
supplied by `dev/docker-compose.yml`. The final stage matters: written in the first stage,
as the ADR-1102 marker is, the file would live in a layer every rebuild reuses and would
report the revision of whichever build first populated the cache.

`scripts/dev/check-container-source.sh` answers the question in both directions.
`--pre-build` refuses to treat a checkout that is behind the reference as a valid build
context, and lists the commits under baked-in paths that the image would be missing.
`--image NAME` reads the marker out of an existing image and reports whether it is
current, stale (naming the missing commits), or unverifiable. A build that never received
`VMAFX_SOURCE_REV` records `unknown`, and `unknown` is reported as **cannot verify**, not
as a pass — an image that cannot say what it holds is not evidence.

`dev/scripts/container-build.sh` makes the correct path the easy one: it runs the
pre-build check, passes the revision it just verified into the build, and re-verifies the
resulting image. `--allow-behind` exists for local experiments and says so loudly.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Record the revision and check the build context (**chosen**) | Answers the question that was actually wrong; catches the stale context before the build rather than after; the image stays self-describing for anything measured in it later | A build argument must be threaded through compose; an image built by a bare `docker compose build` reports `unknown` | — |
| Compare image creation time against the last relevant commit | No Containerfile change; matches rule 15's existing wording | **Does not catch this bug at all.** The image was newer than every commit; it was the source that was old. Automating the rule as written would have returned "current" | It certifies precisely the failure that occurred |
| Rely on the operator to `git pull` first | Zero code | This is the current state, and it failed. The instruction is easy to satisfy in appearance — a rebuild *was* run — and the gap is invisible without checking | An instruction that fails silently is not a control |
| Bake the revision in the first stage, next to the ADR-1102 marker | The two markers would live together | The first stage is cached across rebuilds, so it would record a stale revision — actively worse than no marker, because it would look authoritative | Wrong data is worse than absent data |
| Have the container check itself at start-up | No wrapper script needed | A running container has no network or repo access to compare against, and refusing to start would break every legitimate offline use | Cannot be answered from inside the container |

## Consequences

- **Positive**: a measurement taken inside the container can be attributed to a specific
  revision, which is what makes GPU smoke results usable as retrain-gate evidence. The
  stale-context failure is refused before the 45-minute build rather than discovered after
  it, or not at all.
- **Negative**: `docker compose build dev-mcp` invoked directly still works but yields
  `source_rev=unknown`; the wrapper is what supplies the value. This is a deliberate
  trade — compose cannot run `git` — and the checker reports `unknown` honestly instead of
  guessing.
- **Neutral / follow-ups**: `docs/development/dev-mcp.md` documents the wrapper as the
  default way to rebuild. The hermetic suite
  (`scripts/ci/tests/test-check-container-source.sh`) runs in `Release Script Contract` on
  every PR; its one Docker-dependent case skips when no daemon is reachable.

## References

- req: the maintainer's standing direction to keep the 1.0.0 campaign moving without
  stopping for status; this control exists because a stale container would have produced
  confident, wrong evidence for the epic #1246 retrain gates.
- [ADR-1102](1102-phase4b9-container-only-publishing.md) — the container-vs-host marker
  this complements.
- CLAUDE.md rule 15 — the rebuild-trigger rule whose time-based phrasing this makes
  checkable.
