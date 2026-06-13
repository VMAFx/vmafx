<!-- markdownlint-disable MD060 -->
# ADR-0424: `vmaf-tune benchmark` consumes Phase-A corpora

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris maintainers
- **Tags**: vmaf-tune, cli, benchmark, corpus

## Context

The `.workingdir2` backlog still listed Phase G, the cross-codec corpus
benchmark, as not started. The fork already has enough encode-producing
surfaces (`corpus`, `recommend`, `compare`, `ladder`, `fast`) that another
default encode path would overlap existing commands and increase CI cost.
Operators still need a stable way to answer the post-sweep question: which
encoder clears a target VMAF at the lowest bitrate in an existing Phase-A
JSONL corpus?

## Decision

We will add `vmaf-tune benchmark` as a read-only corpus report. It consumes
`--from-corpus JSONL`, filters successful finite rows, groups by encoder, and
reports each encoder's lowest-bitrate row clearing `--target-vmaf`. Encoders
that do not clear the target remain visible as `unmet` using their closest
VMAF miss. Output formats are `markdown`, `json`, and `csv`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend `compare` | Reuses the existing cross-codec name | `compare` runs Phase-B bisect work; making it also mean "read an existing corpus" blurs runtime expectations | Rejected to keep live encode comparison and offline corpus analysis separate |
| Add benchmark mode to `recommend` | Reuses existing corpus loader | `recommend` returns one row for one predicate, while Phase G needs one summary per encoder plus baseline deltas | Rejected because the output contract is different |
| New `benchmark` subcommand | Clear runtime contract; no new encodes; easy to test from synthetic JSONL | Adds another CLI surface | Chosen; the user-visible surface is small and maps directly to the backlog item |

## Consequences

- **Positive**: Phase-G users can compare codecs from a saved corpus without
  rerunning FFmpeg/libvmaf.
- **Positive**: JSON/CSV output can feed notebooks and dashboards; markdown is
  suitable for PR comments.
- **Negative**: The report inherits corpus coverage bias. If one encoder was
  swept with a narrower CRF/preset grid, its benchmark row is only as good as
  that corpus.
- **Neutral / follow-ups**: BD-rate integration can layer on top once the fork
  standardises interpolation policy across sparse, multi-source corpora.

## References

- `.workingdir2/BACKLOG.md` `VT-OPEN`: Phase G cross-codec corpus benchmark
  not started.
- [ADR-0237](0237-quality-aware-encode-automation.md): `vmaf-tune` umbrella.
- req: "okay in the meantime we can create a new branch and find more
  backlogs etc... bugs or whatever, anything we do is a huge win"
