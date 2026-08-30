<!-- markdownlint-disable MD013 MD025 MD033 MD060 -->
<!-- ADR-0643 stub — claimed by scripts/adr/next-free.sh --claim vmaf-tune-encoder-profile-contract on 2026-05-20T15:31:06Z -->
<!-- Replace this file with the real ADR and rename to 0643-vmaf-tune-encoder-profile-contract.md before committing. -->
<!-- To abandon this claim run: scripts/adr/next-free.sh --release 0643 -->

# ADR-0643: <fill in title>

- **Status**: Proposed
- **Date**: 2026-05-20
- **Deciders**: <fill in>
- **Tags**: <fill in>

## Context

<fill in>

## Decision

<fill in>

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| | | | |

## Consequences

- **Positive**: <fill in>
- **Negative**: <fill in>
- **Neutral / follow-ups**: <fill in>

## References

- See [ADR-0535](0535-adr-atomic-allocator.md) for the original allocator design.
- See [ADR-0628](0628-adr-allocator-remote-aware.md) for the remote-aware extension.
- Source: <req or Q<round>.<q>>

# ADR-0643: vmaf-tune Reports Carry Encoder Profiles

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: vmaf-tune, ffmpeg, docs, profile, cli

## Context

`vmaf-tune compare --format html|both` and `vmaf-tune report` already
produce the human-facing artifact operators inspect after a rate-quality
sweep. The same artifact is the natural hand-off point for production
encoding: it contains the chosen codec rows, source metadata, failures,
and chart context. Before this ADR, the report was mainly a visual card
with a raw JSON appendix; operators still had to translate the winning
row into an FFmpeg command by hand.

The user requested report files that are both easy for humans to read
and rich enough to serve as encoder profiles, plus an FFmpeg-facing
surface and a tool that can read those profiles and encode selected
rows without blindly encoding every codec or ladder rung.

## Decision

Every `ReportData.to_dict()` payload will embed
`encoder_profile.schema == "vmaftune.encoder_profile.v1"`. The profile
records source geometry, run provenance, codec metadata, successful
recommendations, failed rows, ladder samples/rungs, and per-shot rows.
`vmaf-tune encode-profile` reads profile JSON from raw report JSON,
HTML, or Markdown, selects exactly one recommendation via
`--codec`, `--target-vmaf`, and/or `--recommendation-index`, then uses
the existing codec-adapter registry and `EncodeRequest` path to build
and run FFmpeg. FFmpeg patch `0015-vmaf-tune-profile-cli-glue.patch`
adds an advisory `-vmaf-profile <path>` hand-off that points users to
the Python profile reader instead of embedding JSON/profile policy in
FFmpeg.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Put a JSON profile parser directly in FFmpeg | One binary appears to own the workflow | Duplicates vmaf-tune schema migration, codec-adapter defaults, row selection, and report parsing inside a patch stack that must rebase across FFmpeg releases | Too rebase-sensitive for no encoding benefit; FFmpeg should stay the encoder, not the profile policy engine |
| Emit a separate `.profile.json` only | Clean machine payload and simple parser | Humans can lose the sidecar; HTML/Markdown reports stop being self-contained operational artifacts | The user asked for report files that are both human-readable and reusable as profiles |
| Keep raw JSON appendix without a versioned profile | Smallest code change | No stable consumer contract; downstream tools must infer row meaning from report internals | The new `encode-profile` command needs a stable schema discriminator |

## Consequences

- **Positive**: Reports now explain charts in plain language, show
  codec identity chips with links, preserve failed-row context, and
  carry a reusable profile payload.
- **Positive**: Operators can dry-run or run one selected encode from
  a report without repeating the expensive compare sweep.
- **Positive**: FFmpeg patch-stack integration has a discoverable
  `-vmaf-profile` surface while the Python tool remains the source of
  profile truth.
- **Negative**: Report JSON is now a compatibility surface; future
  shape changes need a schema bump or additive-only evolution.
- **Neutral / follow-ups**: Future batch tools can loop over the
  recommendations explicitly for full ladder/materialisation workflows.

## References

- [docs/usage/vmaf-tune.md](../usage/vmaf-tune.md)
- [docs/usage/vmaf-tune-ffmpeg.md](../usage/vmaf-tune-ffmpeg.md)
- [tools/vmaf-tune/AGENTS.md](../../tools/vmaf-tune/AGENTS.md)
- Source: `req` — "make the reports a) perfect for humans and b) with all metadata as well to use them as encoder profiles"
- Source: `req` — "we actually need a new ffmpeg patch and toll that can read the files and encode by them"
