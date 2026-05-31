<!-- markdownlint-disable MD013 MD060 -->
# ADR-0300: `vmaf-tune` HDR-aware encoding + scoring

- **Status**: Accepted (encode-side flags); HDR-VMAF scoring deferred (no fork-local model JSON yet)
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, vmaf-tune, hdr, codec, ffmpeg, fork-local

## Context

`vmaf-tune` Phase A ([ADR-0237](0237-quality-aware-encode-automation.md))
landed an SDR-only grid sweep over `libx264` — the encode invocation
hardcoded BT.709 / gamma-2.4 assumptions and the score invocation
always pointed at `vmaf_v0.6.1` (an SDR-trained model). Bucket #9 of
the Phase A capability audit (Research-0054, the audit body of PR #354)
flagged this as a correctness hazard: feeding a PQ HDR source through
the existing pipeline produces (i) muxed encodes that lose their
mastering-display + max-CLL SEI metadata, and (ii) VMAF scores that
trend artificially low because the SDR model misinterprets the
PQ-coded luma curve.

HDR sources are identifiable from container metadata alone — ffprobe
surfaces `color_transfer` (`smpte2084` for PQ, `arib-std-b67` for HLG),
`color_primaries` (`bt2020`), and the optional mastering-display +
content-light SEI side data. Encoders accept HDR signaling via
codec-private flag families (x265 `-x265-params`, SVT-AV1
`-svtav1-params`, NVENC `-pix_fmt p010le -profile:v main10`,
container-level `-color_*` for everyone else). Netflix maintains an
HDR-trained VMAF model (`vmaf_hdr_v0.6.1.json`) in a separate
research artifact; it has not been ported into this fork.

The action surface for Bucket #9 is therefore split: the
encode-side flag dispatch is a pure refactor of how `vmaf-tune` builds
its ffmpeg argv, while the score-side HDR model swap is gated on a
fork-local port that is out of scope for this PR.

## Decision

We will:

1. Ship a `tools/vmaf-tune/src/vmaftune/hdr.py` module that exposes
   `detect_hdr(path) → HdrInfo | None`, `hdr_codec_args(encoder, info)
   → tuple[str, ...]`, and `select_hdr_vmaf_model() → Path | None`.
2. Wire detection into the corpus driver: when a source's first video
   stream carries PQ or HLG signaling **and** BT.2020 primaries, the
   per-source ffmpeg invocation gets the codec-appropriate HDR flags
   appended to `extra_params`, and the corpus row gains
   `hdr_transfer` / `hdr_primaries` / `hdr_forced` fields
   (schema bumped to v2).
3. Surface four mutually-exclusive CLI modes: `--auto-hdr` (default),
   `--force-sdr`, `--force-hdr-pq`, `--force-hdr-hlg`. The two
   `force-hdr-*` modes synthesise an `HdrInfo` without probing — useful
   for raw YUV reference clips that ffprobe can't carry color metadata
   for.
4. Resolve an HDR VMAF model JSON via `model/vmaf_hdr_*.json` glob
   when one is shipped; when none is found, log a one-shot warning
   and fall back to the configured SDR model. The model port itself
   is a follow-up backlog item; this PR ships the detection +
   resolution scaffolding so the swap is one file drop away.
5. Treat malformed HDR signaling (PQ/HLG transfer with non-BT.2020
   primaries) as SDR — misclassifying SDR as HDR is the dangerous
   failure mode (would inject mismatched primaries into a Rec.709
   encode); misclassifying HDR as SDR is recoverable (encode proceeds
   without HDR signaling, scores trend low, user re-runs with
   `--force-hdr-*`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Detect HDR via libavformat / pyav inside the harness | No subprocess hop; richer side-data API | Adds a Python build dep; libavformat versioning becomes a vmaf-tune concern | ffprobe is already required for SDR pipelines; one more invocation per source is noise vs. the encode wall time |
| Per-codec HDR module under `codec_adapters/` (one HDR file per encoder) | Mirrors the codec-adapter pattern from ADR-0237 | Spreads the HDR contract across N files; flag families overlap heavily (every codec wants the global `-color_*` set) | A single dispatch table keeps the contract auditable in one file; codec-adapter PRs add their own row when they land |
| Skip detection, expose `--hdr-pq` / `--hdr-hlg` flags only | Simpler implementation | User has to read the source's metadata themselves; mixed-corpus runs (some HDR, some SDR sources) need per-source flags | Auto-detect is the demanded UX (`--auto-hdr` defaults true); manual override stays available via `--force-*` |
| Block on the HDR VMAF model port and ship encoding + scoring together | Single coherent PR | Indefinite delay — the model port is a Netflix-research artifact that needs ffmpeg-quality compliance review | Encoder-side correctness is independently valuable; landing it now unblocks corpus runs against HDR sources, scoring port becomes a one-file follow-up |

## Consequences

- **Positive**: HDR sources now produce muxed encodes that retain
  their color signaling; corpus rows record HDR provenance for Phase
  B / C consumers; the codec-adapter PRs (x265, SVT-AV1, NVENC,
  VVenC) inherit a working HDR dispatch the day they land.
- **Negative**: HDR scoring still uses the SDR model — the resulting
  `vmaf_score` values are not directly comparable to SDR scores from
  the same model and trend low for high-luminance regions. Schema v1
  consumers must be updated (the three new keys are additive, but
  `SCHEMA_VERSION` bumped); existing corpus JSONLs remain readable
  but render `hdr_*` keys as missing.
- **Neutral / follow-ups**:
  - Port `vmaf_hdr_v0.6.1.json` from Netflix's HDR research artifact
    into `model/` (backlog item, gated on Netflix-license review).
  - x265 / SVT-AV1 codec adapters (Phase B+) inherit this dispatch
    table without modification — ADR-0235 (codec collision) /
    ADR-0237 (Phase ordering) are unchanged.
  - The schema-v2 row is documented at
    [`docs/usage/vmaf-tune.md` § HDR](../usage/vmaf-tune.md).

## References

- Parent: [ADR-0237](0237-quality-aware-encode-automation.md) (vmaf-tune umbrella spec, Phase A).
- Sibling Phase A audit bucket: PR #354 — Bucket #9 HDR-aware tuning row.
- libvmaf model registry: `model/vmaf_*.json` (no `vmaf_hdr_*.json` shipped yet).
- ffmpeg color flag reference: <https://ffmpeg.org/ffmpeg-codecs.html#libx265>, <https://ffmpeg.org/ffmpeg-codecs.html#SVT_002dAV1>.
- HEVC mastering-display SEI format: ITU-T H.265 (08/2021) D.3.27.
- Source: `req` — Bucket #9 task brief: "HDR sources have specific color metadata in the source — `colorspace=bt2020nc`, `color_trc=smpte2084` (PQ) or `arib-std-b67` (HLG)... ship the encode-side flags only and document that HDR scoring uses the SDR model with a warning."

### Status update 2026-05-09: HDR model status

An autonomous source-or-train research pass exhausted three paths to
close the HDR-VMAF-model gap (full digest in
[research-0089](../research/0089-hdr-vmaf-model-search.md)):

- **Path A (source from elsewhere) — failed.** `Netflix/vmaf/model/`
  still ships no `vmaf_hdr_*` artifact (verified via
  `gh api repos/Netflix/vmaf/contents/model` on 2026-05-09);
  collaborator `li-zhi` on
  [issue #645](https://github.com/Netflix/vmaf/issues/645) confirmed
  "no timeline" and the most recent public statement (CSI Magazine
  2023-11-30) was "before the official release." Hugging Face,
  GitHub-wide code search, and academic releases (HDRMAX —
  different algorithm, sklearn pickles, not libvmaf-JSON-loadable)
  all returned negative findings. No BSD-3-Clause-Plus-Patent-
  compatible, libvmaf-JSON-loadable HDR VMAF model exists publicly
  as of 2026-05-09.
- **Path B (train a fork-owned model) — deferred.** All five
  candidate subjective HDR corpora (LIVE-HDR, LIVE-HDRvsSDR,
  LIVE-TMHDR, ESPL-LIVE HDR, ITU-T SDR-vs-HDR) are gated behind
  manual access forms or unavailable; redistribution of
  derived weights under BSD-3-Clause-Plus-Patent is unclear; and
  the multi-day training run exceeded the research-pass budget.
  Filed as a follow-up backlog row in
  [`docs/state.md`](../state.md).
- **Path C (degrade gracefully + document) — chosen.** Ship a
  `model/vmaf_hdr_model_card.md` that loudly documents the SDR
  fallback. **No fabricated model weights are introduced.** The
  resolver glob (`vmaf_hdr_*.json`) is unchanged and continues to
  return `None`; the fallback that ADR-0300 already documented is
  unchanged at runtime. The only delta is that the fallback is now
  *discoverable* from the `model/` directory listing.

This status update does not change the ADR's Decision; the ADR's
"HDR-VMAF scoring deferred" status is preserved. The empirical
SDR-on-HDR delta measurement called out in the digest's follow-up
backlog is the smaller next step that does not require gated
corpora.

### Status update 2026-05-08: iter_rows integration + transfer-aware model slot

Phase-A audit item HP-2 closed. The original PR landed `hdr.py` and
the four CLI flags but never wired `detect_hdr` /
`hdr_codec_args` / `select_hdr_vmaf_model` into
`corpus.iter_rows` — `grep -nE "from.*\.hdr|import.*hdr"
tools/vmaf-tune/src/vmaftune/*.py` returned zero hits. PQ sources
silently encoded as SDR with PQ metadata stripped. The follow-up
PRs wire the integration:

- `corpus.iter_rows` now resolves the effective HDR mode once per
  source via the new `_resolve_hdr` helper, then injects
  `hdr_codec_args(opts.encoder, info)` into `EncodeRequest.extra_params`
  and swaps in an HDR VMAF model when `select_hdr_vmaf_model()`
  returns one (else logs a one-shot warning and keeps the SDR
  model).
- `_row_for` populates the schema-v3 `hdr_transfer` /
  `hdr_primaries` / `hdr_forced` columns so Phase B/C consumers
  can distinguish detected vs user-asserted HDR rows.
- `SCHEMA_VERSION` bumped 2 → 3 (additive); the original ADR
  body claimed "schema bumped to v2" but the v2 bump consumed the
  `clip_mode` slot first — the HDR triple lands as v3.
- The two integration tests previously gated by
  `_HDR_ITER_ROWS_DEFERRED` (`test_corpus_emits_hdr_fields_when_source_is_hdr`,
  `test_corpus_force_sdr_skips_hdr_path`) are un-skipped and pass.
- `select_hdr_vmaf_model()` now accepts an optional ``transfer`` keyword
  (``"pq"`` / ``"hlg"`` / ``None``) and prefers the canonical filename
  ``vmaf_hdr_v0.6.1.json`` when present; falls back to the
  ``vmaf_hdr_*.json`` glob for forward compatibility with future
  Netflix revisions. A new ``hdr_model_name_for(transfer)`` helper
  exposes the dispatch table. A ``reset_hdr_model_warning()`` test hook
  clears the once-per-process warning flag.

The HDR VMAF scoring story is unchanged — model port still
backlog. Encode-side correctness is now active for every PQ /
HLG source the harness sees. Upstream Netflix/vmaf master `model/`
does **not** ship ``vmaf_hdr_v0.6.1.json`` as of 2026-05-08;
a fork-local license review is the gating follow-up.
