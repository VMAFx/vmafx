<!-- markdownlint-disable MD060 -->
# ADR-0527: Accept pre-extracted BVI-DVC YUVs via `--bvi-dir`

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `ai`, `corpus`, `cli`, `docs`

## Context

`ai/scripts/bvi_dvc_to_full_features.py` previously accepted only a single
input mode: `--bvi-zip`, pointing at the full `BVI-DVC Part 1.zip` archive
(~84 GiB). For each clip it streamed the `.mp4` out of the zip, decoded to
raw YUV, encoded a distorted side, ran libvmaf, and deleted the temporary
`.mp4`.

The user has 192 GB of raw BVI-DVC YUV files already extracted on disk.
Requiring the zip forces an unnecessary streaming-extraction step on every
invocation and prevents using the pre-decoded YUV files directly.
A second input mode that points at a directory removes this friction and
enables faster iteration on the feature-extraction step.

## Decision

We will add a `--bvi-dir PATH` argument to `bvi_dvc_to_full_features.py`,
mutually exclusive with `--bvi-zip`. When `--bvi-dir` is supplied the script
enumerates `.mp4` and `.yuv` files in the directory using the BVI-DVC filename
convention to derive resolution, framerate, bit-depth, and tier. For `.yuv`
inputs the decode step is skipped entirely — the file is fed to libvmaf as the
reference directly. For `.mp4` inputs the existing decode path is reused. No
files are deleted after processing. The `--bvi-zip` default (legacy env-var
fallback) is preserved when neither flag is provided.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `--bvi-dir` as implemented | Zero extraction overhead for pre-decoded YUV; MP4 inputs reuse existing decode path; backward-compatible (neither flag → zip default) | Tier is inferred from resolution rather than filename prefix, so clips with non-standard resolutions are skipped with a warning | Chosen — the four BVI-DVC canonical resolutions are a closed set; the warning + skip is the right safety valve |
| Auto-scan a well-known search path (e.g. `.workingdir2/bvi-dvc-extracted/`) | No extra flag required | Silently wrong if the user's extraction landed somewhere else; non-obvious from `--help`; not composable with other input sources | Rejected — an explicit flag is clearer and safer |
| Single `--bvi-input` flag that auto-detects zip vs. directory | Fewer flags; slightly simpler `--help` | Ambiguous if the path points at a directory named `*.zip`; makes the mutual-exclusion constraint implicit rather than explicit | Rejected — explicit `add_mutually_exclusive_group()` is the right argparse pattern here |
| Modify `--bvi-zip` to also accept a directory | Minimal surface change | Semantically misleading name; breaks any caller that checks `args.bvi_zip.is_file()` | Rejected |

## Consequences

- **Positive**: Users with pre-extracted corpora can skip the streaming
  extraction step entirely; `.yuv` inputs also skip the decode step,
  saving further wall time.
- **Positive**: The four-tier resolution map (`_RES_TO_TIER`) makes the
  tier assignment explicit and auditable; unknown resolutions emit a
  clear warning.
- **Positive**: Backward-compatible — callers that omit both flags continue
  to use the zip-based path via the env-var / hard-coded default.
- **Negative**: The `--bvi-dir` path infers the bit-depth from the
  filename (`_Nbit_` component). Files with non-standard naming (e.g. no
  `_10bit_` component) are skipped rather than probed.
- **Neutral / follow-ups**: The `_DirEntry` class mirrors `zipfile.ZipInfo`
  just enough for the shared iteration loop; if the script gains more
  entry attributes (e.g. frame count) both classes will need updating.

## References

- User direction: the user has 192 GB of raw BVI-DVC YUVs on disk and
  requested a `--bvi-dir` mode so the zip extraction step can be skipped.
- Related ADR: [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) — original
  BVI-DVC ingestion pipeline decision.
- Implementation PR: `feat/bvi-dvc-pre-extracted-input`.
