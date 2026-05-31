<!-- markdownlint-disable MD060 -->
# ADR-0644: Add vmaf-tune codec runtime variants

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `ffmpeg`, `cli`, `docs`, `fork-local`

## Context

[ADR-0641](0641-dev-container-encoder-probe-hardening.md) closed the dev-container
probe/runtime gaps but left one codec-identity gap open: the fork needs to compare
mainline SVT-AV1 and `juliobbv-p/svt-av1-hdr` side by side. The HDR fork is an
SVT-AV1 library/runtime variant, not a distinct FFmpeg encoder exposed under a
new `-c:v` name. Its public README points users at community FFmpeg builds, and
its FFmpeg examples still invoke `-c:v libsvtav1` with SVT-specific parameters.

The previous `vmaf-tune compare` model had one global `--ffmpeg-bin` for the
whole run. That was sufficient for comparing different FFmpeg encoder names
(`libx265` vs `libsvtav1` vs `av1_qsv`), but not for comparing two builds that
both surface as `libsvtav1`. Reusing the existing `libsvtav1` token with a
different global FFmpeg binary made the report ambiguous; adding a fake
`libsvtav1-hdr` adapter would have lied about the actual FFmpeg surface and
duplicated adapter policy.

## Decision

We will represent compare-time runtime variants as `ADAPTER@VARIANT` display
tokens. The substring before `@` is the real codec-adapter key used for argv
generation and FFmpeg probing; the full token is the report label. Operators bind
a variant to its FFmpeg runtime with `--encoder-ffmpeg-bin TOKEN=PATH`; unbound
tokens use the existing global `--ffmpeg-bin`. For example,
`libsvtav1@svt-av1-hdr` still routes through the `libsvtav1` adapter and FFmpeg
`-c:v libsvtav1`, but its probes and encodes can use an FFmpeg binary linked
against the HDR fork.

Compare JSON/CSV rows now carry `adapter`, `runtime_variant`, and `ffmpeg_bin`
metadata alongside the existing user-facing `codec` label so reports remain
auditable after the run.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `libsvtav1-hdr` as a codec adapter | Simple to select in `--encoders`; no new CLI flag | Falsely implies FFmpeg exposes a distinct encoder; duplicates `libsvtav1` policy; makes adapter registry carry runtime identity | The upstream HDR fork still uses the `libsvtav1` FFmpeg wrapper, so runtime identity belongs outside the adapter registry |
| Keep only global `--ffmpeg-bin` | No code change | Cannot compare mainline and HDR-linked SVT-AV1 in the same sweep; report rows both appear as `libsvtav1` | The requested workflow is side-by-side comparison in one report |
| Require wrapper scripts named as fake encoders | Lets shell users hide runtime differences externally | Moves provenance out of the report; hard to audit which binary produced which row; still needs a label mechanism | The report must carry enough metadata to function as an encoder profile |

## Consequences

- **Positive**: `vmaf-tune compare` and `compare --no-bisect` can compare
  mainline `libsvtav1` against `libsvtav1@svt-av1-hdr` in one run while keeping
  the base adapter contract honest. JSON/CSV consumers can distinguish display
  label, adapter key, variant label, and FFmpeg runtime path.
- **Negative**: Compare now has another CLI knob and a slightly wider row
  schema. Programmatic consumers that hard-code compare CSV column order need to
  accept the three new provenance columns.
- **Neutral / follow-ups**: Corpus generation still records one `encoder` per
  row and is not widened in this PR. If runtime variants become useful for full
  corpus generation, the same `ADAPTER@VARIANT` contract can be promoted there
  with a schema bump.

## References

- Research digest: [0644-vmaf-tune-codec-runtime-variants](../research/0644-vmaf-tune-codec-runtime-variants.md).
- `tools/vmaf-tune/src/vmaftune/encoder_runtime.py`.
- `tools/vmaf-tune/src/vmaftune/cli.py` compare runtime dispatch.
- `tools/vmaf-tune/src/vmaftune/compare.py` row-schema metadata and encoder probe.
- Source: `req` — "nvm, fuck psy, this is the new one [juliobbv-p/svt-av1-hdr](https://github.com/juliobbv-p/svt-av1-hdr/)"
- Source: `req` — "well go on we had more backlogs no?"
